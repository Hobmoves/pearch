#include "index/index.h"
#include "index/database.h"
#include "ui/dashboard.h"
#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <chrono>
#include <iostream>
#include <sstream>
#include <deque>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

using namespace std::chrono;

static std::optional<std::wstring> g_test_db_override;

static std::wstring get_default_db_path() {
    if (g_test_db_override.has_value()) return *g_test_db_override;
    // Use %LOCALAPPDATA%\pearch\index.sqlite
    wchar_t* buf = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &buf))) {
        std::wstring path(buf);
        CoTaskMemFree(buf);
        std::filesystem::path p(path);
        p /= L"pearch";
        std::filesystem::create_directories(p);
        p /= L"index.sqlite";
        return p.wstring();
    }
    // fallback to current dir
    return std::filesystem::absolute(L"index.sqlite").wstring();
}


static std::string utf8_from_wstring(const std::wstring& w) {
    if (w.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    if (size <= 0) return {};
    std::string out(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), size, NULL, NULL);
    return out;
}

static std::wstring utf8_to_wstring(const std::string& s) {
    if (s.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    if (size <= 0) return {};
    std::wstring out(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), size);
    return out;
}

namespace Index {

static Database g_db;

void set_db_path_for_tests(const std::wstring& path) {
    g_test_db_override = path;
}

bool initialize_database() {
    std::wstring dbpath = get_default_db_path();
    if (!g_db.open(dbpath)) return false;

    // Create tables
    const char* schema = R"SQL(
    PRAGMA journal_mode=WAL;
    PRAGMA synchronous=NORMAL;
    CREATE TABLE IF NOT EXISTS entries (
        path TEXT PRIMARY KEY,
        filename TEXT,
        extension TEXT,
        is_directory INTEGER,
        size INTEGER,
        mtime INTEGER
    );
    CREATE INDEX IF NOT EXISTS idx_entries_filename ON entries(filename COLLATE NOCASE);
    CREATE INDEX IF NOT EXISTS idx_entries_path ON entries(path COLLATE NOCASE);
    CREATE TABLE IF NOT EXISTS metadata (key TEXT PRIMARY KEY, value TEXT);
    )SQL";

    return g_db.execute(schema);
}

bool build_index(const std::wstring& path, bool verbose, Scanner::ScanResult& out_scan_result, double& elapsed_seconds, bool no_color, bool quiet) noexcept {
    if (!initialize_database()) return false;

    const char* insert_sql = R"SQL(
    INSERT INTO entries (path, filename, extension, is_directory, size, mtime)
    VALUES (?, ?, ?, ?, ?, ?)
    ON CONFLICT(path) DO UPDATE SET
        filename=excluded.filename,
        extension=excluded.extension,
        is_directory=excluded.is_directory,
        size=excluded.size,
        mtime=excluded.mtime
    ;
    )SQL";

    // concurrent queue and writer
    std::deque<FileEntry> queue;
    std::mutex q_mtx;
    std::condition_variable q_cv;
    std::atomic<bool> done{false};

    UI::Dashboard dashboard(no_color, quiet);
    dashboard.start();

    // counters
    std::atomic<uint64_t> files_seen{0};
    std::atomic<uint64_t> dirs_seen{0};
    std::atomic<uint64_t> errors_seen{0};
    std::atomic<uint64_t> entries_written{0};
    dashboard.files_seen = 0;
    dashboard.dirs_seen = 0;
    dashboard.errors = 0;
    dashboard.entries_written = 0;

    auto start = high_resolution_clock::now();

    // writer thread
    std::thread writer([&]{
        sqlite3_stmt* stmt = g_db.prepare(insert_sql);
        if (!stmt) return;
        const size_t batch_size = 512;
        std::vector<FileEntry> batch;
        batch.reserve(batch_size);
        while (!done || !queue.empty()) {
            {
                std::unique_lock<std::mutex> lk(q_mtx);
                q_cv.wait_for(lk, std::chrono::milliseconds(200), [&]{ return !queue.empty() || done.load(); });
                while (!queue.empty() && batch.size() < batch_size) {
                    batch.push_back(std::move(queue.front())); queue.pop_front();
                }
            }
            if (!batch.empty()) {
                g_db.execute("BEGIN TRANSACTION;");
                for (auto &e : batch) {
                    sqlite3_reset(stmt);
                    sqlite3_clear_bindings(stmt);
                    std::string path_u8 = utf8_from_wstring(e.full_path);
                    std::string filename_u8 = utf8_from_wstring(e.filename);
                    std::string ext_u8 = utf8_from_wstring(e.extension);
                    sqlite3_bind_text(stmt, 1, path_u8.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 2, filename_u8.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 3, ext_u8.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int(stmt, 4, e.is_directory ? 1 : 0);
                    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)e.file_size);
                    ULARGE_INTEGER ui{}; ui.LowPart = e.last_write_time.dwLowDateTime; ui.HighPart = e.last_write_time.dwHighDateTime;
                    uint64_t ft100ns = ui.QuadPart;
                    uint64_t epoch100ns = ft100ns - 116444736000000000ULL;
                    uint64_t msecs = epoch100ns / 10000ULL;
                    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)msecs);
                    int rc = sqlite3_step(stmt);
                    if (rc != SQLITE_DONE) {
                        if (verbose) std::cerr << "sqlite insert error: " << sqlite3_errmsg(g_db.raw()) << "\n";
                        dashboard.errors.fetch_add(1);
                    } else {
                        entries_written.fetch_add(1);
                        dashboard.entries_written.fetch_add(1);
                    }
                }
                g_db.execute("COMMIT;");
                batch.clear();
            }
        }
        sqlite3_finalize(stmt);
    });

    // scanner callback pushes to queue
    auto cb = [&](const FileEntry& e) {
        if (e.is_directory) { dirs_seen.fetch_add(1); dashboard.dirs_seen.fetch_add(1); }
        else { files_seen.fetch_add(1); dashboard.files_seen.fetch_add(1); }
        // update current path for dashboard
        dashboard.set_current_path(e.full_path);
        {
            std::lock_guard<std::mutex> lk(q_mtx);
            queue.push_back(e);
        }
        q_cv.notify_one();
    };

    auto scan_result = Scanner::scan(path, cb, verbose);

    // scanning done
    done = true;
    q_cv.notify_one();
    if (writer.joinable()) writer.join();

    auto end = high_resolution_clock::now();
    elapsed_seconds = duration<double>(end - start).count();
    out_scan_result = scan_result;

    // update metadata
    std::string now = std::to_string((uint64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    std::string update_meta = "REPLACE INTO metadata (key, value) VALUES ('last_indexed', '" + now + "');";
    g_db.execute(update_meta.c_str());

    dashboard.stop();

    return true;
}

std::vector<SearchResult> search(const std::string& query, size_t limit, double& query_time_ms) noexcept {
    std::vector<SearchResult> out;
    if (!initialize_database()) return out;

    // Escape LIKE wildcards in query to avoid unintended matches
    auto escape_like = [](const std::string& s) {
        std::string r;
        r.reserve(s.size() * 2);
        for (char c : s) {
            if (c == '%' || c == '_' || c == '\\') {
                r.push_back('\\');
            }
            r.push_back(c);
        }
        return r;
    };

    std::string q_esc = escape_like(query);
    std::string q_exact = query;
    std::string q_prefix = q_esc + "%";
    std::string q_contains = "%" + q_esc + "%";

        const char* sql = R"SQL(
        SELECT path, filename, is_directory,
            CASE
                WHEN filename = ? COLLATE NOCASE THEN 0
                WHEN filename COLLATE NOCASE LIKE ? ESCAPE '\' THEN 1
                WHEN filename COLLATE NOCASE LIKE ? ESCAPE '\' THEN 2
                WHEN path COLLATE NOCASE LIKE ? ESCAPE '\' THEN 3
                ELSE 4 END as rank,
            LENGTH(filename) as fname_len,
            LENGTH(path) as path_len,
            is_directory
        FROM entries
        WHERE filename = ? COLLATE NOCASE OR filename COLLATE NOCASE LIKE ? ESCAPE '\' OR filename COLLATE NOCASE LIKE ? ESCAPE '\' OR path COLLATE NOCASE LIKE ? ESCAPE '\'
        ORDER BY rank ASC, fname_len ASC, path_len ASC, is_directory ASC, filename ASC
        LIMIT ?
        ;
        )SQL";

    // Debug: (can be removed) inspect SQL and parameters when troubleshooting
    // Print database path and parameters for debugging
    (void)q_esc; (void)q_exact; (void)q_prefix; (void)q_contains; (void)sql;

    sqlite3_stmt* stmt = g_db.prepare(sql);
    if (!stmt) return out;

    auto start = high_resolution_clock::now();

    // bind params
    sqlite3_bind_text(stmt, 1, q_exact.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, q_prefix.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, q_contains.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, q_contains.c_str(), -1, SQLITE_TRANSIENT);
    // WHERE params
    sqlite3_bind_text(stmt, 5, q_exact.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, q_prefix.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, q_contains.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, q_contains.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 9, (int)limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* path = sqlite3_column_text(stmt, 0);
        const unsigned char* filename = sqlite3_column_text(stmt, 1);
        int isdir = sqlite3_column_int(stmt, 2);
        SearchResult r;
        r.path = path ? reinterpret_cast<const char*>(path) : "";
        r.filename = filename ? reinterpret_cast<const char*>(filename) : "";
        r.is_directory = isdir != 0;
        out.push_back(r);
    }
    // debug: print fetched count
    // std::cerr << "[debug] fetched rows: " << out.size() << "\n";

    auto end = high_resolution_clock::now();
    query_time_ms = duration<double, std::milli>(end - start).count();
    sqlite3_finalize(stmt);
    return out;
}

IndexStats stats() noexcept {
    IndexStats s{};
    if (!initialize_database()) return s;
    // count entries
    sqlite3_stmt* st = g_db.prepare("SELECT COUNT(*) FROM entries;");
    if (st) {
        if (sqlite3_step(st) == SQLITE_ROW) s.entries = (uint64_t)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    // files vs dirs
    st = g_db.prepare("SELECT SUM(CASE WHEN is_directory=0 THEN 1 ELSE 0 END), SUM(CASE WHEN is_directory=1 THEN 1 ELSE 0 END) FROM entries;");
    if (st) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            s.files = (uint64_t)sqlite3_column_int64(st, 0);
            s.directories = (uint64_t)sqlite3_column_int64(st, 1);
        }
        sqlite3_finalize(st);
    }
    // db size
    try {
        auto dbpath = get_default_db_path();
        std::error_code ec;
        auto sz = std::filesystem::file_size(dbpath, ec);
        if (!ec) s.db_size_bytes = (uint64_t)sz;
    } catch (...) {}

    // last indexed
    st = g_db.prepare("SELECT value FROM metadata WHERE key='last_indexed';");
    if (st) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* v = sqlite3_column_text(st, 0);
            if (v) s.last_indexed = reinterpret_cast<const char*>(v);
        }
        sqlite3_finalize(st);
    }
    return s;
}

} // namespace Index
