#include "index/database.h"
#include <windows.h>
#include <shlobj.h>
#include <iostream>

Database::~Database() {
    close();
}

static std::string utf8_from_wstring(const std::wstring& w) {
    if (w.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    if (size <= 0) return {};
    std::string out(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), size, NULL, NULL);
    return out;
}

bool Database::open(const std::wstring& path) noexcept {
    close();
    std::string utf8 = utf8_from_wstring(path);
    int rc = sqlite3_open_v2(utf8.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        std::cerr << "sqlite open failed: " << sqlite3_errmsg(db_) << "\n";
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    return true;
}

void Database::close() noexcept {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool Database::execute(const char* sql) noexcept {
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::cerr << "sqlite exec error: " << (err ? err : "unknown") << "\n";
        sqlite3_free(err);
        return false;
    }
    return true;
}

sqlite3_stmt* Database::prepare(const char* sql) noexcept {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "sqlite prepare error: " << sqlite3_errmsg(db_) << "\n";
        return nullptr;
    }
    return stmt;
}
