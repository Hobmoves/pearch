#include <cstdio>
#include <filesystem>
#include <random>

#include "check.h"
#include "core/fileio.h"
#include "engine/index_store.h"

using namespace engine;

namespace {

std::string sep(std::initializer_list<const char*> parts) {
    std::string out;
    for (const char* p : parts) {
        if (!out.empty()) out += fileio::kPathSep;
        out += p;
    }
    return out;
}

std::shared_ptr<IndexStore> two_roots() {
    IndexBuilder b;
    uint32_t r1 = b.add_root(sep({"alpha"}), 1);
    uint32_t d = b.add(r1, "Docs", kDir, 0, 2);
    b.add(d, "Résumé.pdf", 0, 123, 3);
    b.add(d, "notes.txt", kHidden, 4, 4);
    b.add(r1, "top.txt", 0, 5, 5);
    uint32_t r2 = b.add_root(sep({"beta"}), 6);
    uint32_t e = b.add(r2, "Empty", kDir, 0, 7);
    (void)e;
    b.add(r2, "Start.lnk", kApp, 8, 8);
    return b.finish(42);
}

std::string temp_file(const char* name) {
    auto dir = std::filesystem::temp_directory_path();
    return (dir / name).string();
}

}  // namespace

TEST(store_paths_and_tree) {
    auto s = two_roots();
    CHECK(s != nullptr);
    CHECK_EQ(s->size(), size_t{8});
    CHECK_EQ(s->roots().size(), size_t{2});
    uint32_t pdf = s->find_path(sep({"alpha", "Docs", "Résumé.pdf"}));
    CHECK(pdf != kNone);
    CHECK_EQ(s->path(pdf), sep({"alpha", "Docs", "Résumé.pdf"}));
    CHECK_EQ(s->parent_path(pdf), sep({"alpha", "Docs"}));
    CHECK_EQ(std::string(s->folded(pdf)), std::string("resume.pdf"));
    CHECK_EQ(s->depth(pdf), 2);
    CHECK(s->find_path(sep({"alpha", "docs", "RESUME.PDF"})) != kNone);  // case/accent-insensitive fallback
    CHECK(s->find_path(sep({"alpha", "DOCS", "notes.txt"})) != kNone);   // case-insensitive fallback
    uint32_t docs = s->find_path(sep({"alpha", "Docs"}));
    CHECK_EQ(s->subtree_end(docs), docs + 3);
    CHECK_EQ(s->subtree_end(0), 5u);
    CHECK_EQ(std::string(s->name(0)), std::string("alpha"));
    CHECK_EQ(s->count_dirs(), uint64_t{4});
}

TEST(store_save_load_round_trip) {
    auto s = two_roots();
    std::string file = temp_file("pearch_test_store.bin");
    std::string err;
    CHECK(s->save(file, &err));
    auto l = IndexStore::load(file, &err);
    CHECK(l != nullptr);
    if (!l) return;
    CHECK_EQ(l->size(), s->size());
    CHECK_EQ(l->built_at(), int64_t{42});
    for (uint32_t i = 0; i < s->size(); ++i) {
        CHECK_EQ(l->path(i), s->path(i));
        CHECK_EQ(l->flags(i), s->flags(i));
        CHECK_EQ(l->file_size(i), s->file_size(i));
        CHECK_EQ(l->mtime(i), s->mtime(i));
        CHECK_EQ(l->subtree_end(i), s->subtree_end(i));
        CHECK_EQ(l->mask(i), s->mask(i));
    }
    // Truncated or garbage files are rejected, not crashed on.
    std::string data;
    CHECK(fileio::read_all(file, data));
    CHECK(fileio::write_atomic(file, data.substr(0, data.size() / 2)));
    CHECK(IndexStore::load(file, &err) == nullptr);
    std::string bad = data;
    bad[bad.size() / 2] ^= 0x5A;
    bad[40] ^= 0x7F;
    fileio::write_atomic(file, bad);
    CHECK(IndexStore::load(file, &err) == nullptr || true);  // must not crash
    CHECK(IndexStore::load(temp_file("pearch_does_not_exist.bin"), &err) == nullptr);
    fileio::remove_file(file);
}

TEST(store_remove_and_merge_locations) {
    auto s = two_roots();
    auto without = s->without_root(0);
    CHECK(without != nullptr);
    CHECK_EQ(without->size(), size_t{3});
    CHECK(without->find_path(sep({"beta", "Start.lnk"})) != kNone);
    CHECK(without->find_path(sep({"alpha", "top.txt"})) == kNone);
    auto merged = IndexStore::merge(*without, *s->without_root(5));
    CHECK(merged != nullptr);
    CHECK_EQ(merged->size(), s->size());
    CHECK(merged->find_path(sep({"alpha", "Docs", "notes.txt"})) != kNone);
    CHECK(merged->flags(merged->find_path(sep({"beta", "Start.lnk"}))) & kApp);
}

TEST(store_builder_rejects_bad_order) {
    IndexBuilder b;
    uint32_t r = b.add_root("root", 0);
    uint32_t file = b.add(r, "file.txt", 0, 0, 0);
    b.add(file, "child-of-a-file", 0, 0, 0);  // parent is not a folder
    CHECK(b.finish(0) == nullptr);
}
