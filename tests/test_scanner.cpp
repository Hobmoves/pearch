#include <chrono>
#include <filesystem>
#include <fstream>

#include "check.h"
#include "core/fileio.h"
#include "core/text.h"
#include "engine/searcher.h"
#include "fs/scanner.h"

namespace fs = std::filesystem;
using namespace engine;

namespace {

struct TempTree {
    fs::path root;
    bool has_symlink = false;
    TempTree() {
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() / ("pearch_scan_" + std::to_string(stamp));
        fs::create_directories(root / "b" / "d");
        fs::create_directories(root / "node_modules" / "pkg");
        fs::create_directories(root / "apps");
        fs::create_directories(root / "skipme");
        touch(root / "a.txt", 1);
        touch(root / "b" / "c.md", 10);
        touch(root / "b" / "d" / "e.bin", 1234);
        touch(root / "node_modules" / "pkg" / "x.js", 5);
        touch(root / fs::path(text::to_wide("Café.txt")), 3);
        touch(root / "apps" / "Tool.lnk", 7);
        touch(root / "apps" / "tool.exe", 7);
        touch(root / "skipme" / "hidden.txt", 7);
        std::error_code ec;
        fs::create_directory_symlink(root / "b", root / "link_to_b", ec);
        has_symlink = !ec;
    }
    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    static void touch(const fs::path& p, size_t bytes) {
        std::ofstream f(p, std::ios::binary);
        f << std::string(bytes, 'x');
    }
    std::string str(const fs::path& p) const { return fileio::absolute(text::to_utf8(p.wstring())); }
};

}  // namespace

TEST(scanner_indexes_a_tree) {
    TempTree t;
    scan::Options opt;
    opt.roots = {t.str(t.root)};
    opt.skip_names = {"node_modules"};
    opt.skip_paths = {t.str(t.root / "skipme")};
    opt.app_paths = {t.str(t.root / "apps")};
    scan::Progress p;
    auto s = scan::build(opt, p, 1);
    CHECK(s != nullptr);
    if (!s) return;
    CHECK(p.finished.load());
    CHECK_EQ(s->size(), size_t{1} + p.entries());

    uint32_t e = s->find_path(t.str(t.root / "b" / "d" / "e.bin"));
    CHECK(e != kNone);
    if (e != kNone) {
        CHECK_EQ(s->file_size(e), uint64_t{1234});
        CHECK(s->mtime(e) > 0);
        CHECK_EQ(s->path(e), t.str(t.root / "b" / "d" / "e.bin"));
    }
    // Skipped folders are listed but not descended into.
    CHECK(s->find_path(t.str(t.root / "node_modules")) != kNone);
    CHECK(s->find_path(t.str(t.root / "node_modules" / "pkg")) == kNone);
    CHECK(s->find_path(t.str(t.root / "skipme")) != kNone);
    CHECK(s->find_path(t.str(t.root / "skipme" / "hidden.txt")) == kNone);
    // Apps.
    uint32_t lnk = s->find_path(t.str(t.root / "apps" / "Tool.lnk"));
    uint32_t exe = s->find_path(t.str(t.root / "apps" / "tool.exe"));
    CHECK(lnk != kNone && (s->flags(lnk) & kApp));
    CHECK(exe != kNone && (s->flags(exe) & kApp));
    CHECK(!(s->flags(s->find_path(t.str(t.root / "a.txt"))) & kApp));
    // Symlinked folders are recorded but not followed (no duplicates).
    if (t.has_symlink) {
        uint32_t link = s->find_path(t.str(t.root / "link_to_b"));
        CHECK(link != kNone);
        if (link != kNone) CHECK_EQ(s->subtree_end(link), link + 1);
    }
    // Accent-insensitive search over a scanned tree.
    ThreadPool pool;
    Searcher searcher(pool);
    SearchRequest req;
    req.text = "cafe";
    auto r = searcher.search(s, req);
    CHECK_EQ(r.size(), size_t{1});
    if (!r.empty()) CHECK_EQ(std::string(r.store->name(r.id(0))), std::string("Café.txt"));
}

TEST(scanner_handles_missing_roots_and_cancel) {
    TempTree t;
    scan::Options opt;
    opt.roots = {t.str(t.root / "does-not-exist"), t.str(t.root)};
    scan::Progress p;
    auto s = scan::build(opt, p, 1);
    CHECK(s != nullptr);
    CHECK(p.errors.load() >= 1);
    CHECK(s && s->roots().size() == 1);

    scan::Progress cancelled;
    cancelled.cancel = true;
    CHECK(scan::build(opt, cancelled, 1) == nullptr);
}
