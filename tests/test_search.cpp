#include <algorithm>
#include <random>
#include <set>

#include "check.h"
#include "core/fileio.h"
#include "core/format.h"
#include "core/text.h"
#include "engine/searcher.h"

using namespace engine;

namespace {

const int64_t kNow = fmt::now_ms();
constexpr int64_t kDay = 86400000LL;

// The fixture from pearch 0.1's test suite, plus a few extras.
struct Fixture {
    std::shared_ptr<IndexStore> store;
    Fixture() {
        IndexBuilder b;
        std::string root = std::string("pearch_fixture");
        uint32_t r = b.add_root(root, kNow);
        b.add(r, "normal.txt", 0, 10, kNow - 40 * kDay);
        b.add(r, "MiXeD.Case", 0, 10, kNow - 40 * kDay);
        b.add(r, "space name.txt", 0, 10, kNow - 40 * kDay);
        b.add(r, "percent%file.txt", 0, 10, kNow - 40 * kDay);
        b.add(r, "underscore_file_.txt", 0, 10, kNow - 40 * kDay);
        uint32_t nested = b.add(r, "nested_dir", kDir, 0, kNow - 40 * kDay);
        b.add(nested, "dir_with_foo", kDir, 0, kNow - 40 * kDay);
        b.add(nested, "inner.cpp", 0, 2048, kNow - 2 * kDay);
        b.add(nested, "cmake.txt", 0, 300, kNow - 1000);
        b.add(r, "foo", 0, 1, kNow - 40 * kDay);
        b.add(r, "foo_bar", 0, 1, kNow - 40 * kDay);
        b.add(r, "xfoo", 0, 1, kNow - 40 * kDay);
        b.add(r, "ab", 0, 1, kNow - 40 * kDay);
        b.add(r, "abc", 0, 1, kNow - 40 * kDay);
        b.add(r, "Quarterly Report Final.pdf", 0, 5 * 1024 * 1024, kNow - 3 * kDay);
        b.add(r, "Google Chrome.lnk", kApp, 2000, kNow - 90 * kDay);
        store = b.finish(kNow);
    }
};

std::vector<std::string> names(SearchResult& r) {
    std::vector<std::string> out;
    for (size_t k = 0; k < r.size(); ++k) out.emplace_back(r.store->name(r.id(k)));
    return out;
}

int pos(const std::vector<std::string>& v, const std::string& s) {
    auto it = std::find(v.begin(), v.end(), s);
    return it == v.end() ? -1 : static_cast<int>(it - v.begin());
}

SearchResult run(const std::shared_ptr<IndexStore>& s, const std::string& q, TypeFilter tab = TypeFilter::All,
                 uint32_t scope = kNone, std::shared_ptr<const Boosts> boosts = nullptr) {
    static ThreadPool pool;
    Searcher searcher(pool);
    SearchRequest req;
    req.text = q;
    req.tab = tab;
    req.scope = scope;
    req.now_ms = kNow;
    req.boosts = std::move(boosts);
    return searcher.search(s, req);
}

}  // namespace

TEST(search_basic_matches) {
    Fixture f;
    auto r1 = run(f.store, "normal.txt");
    CHECK(!r1.empty());
    CHECK_EQ(names(r1)[0], std::string("normal.txt"));
    auto r2 = run(f.store, "inner.cpp");
    CHECK(!r2.empty() && names(r2)[0] == "inner.cpp");
    auto r3 = run(f.store, "space name");
    CHECK(pos(names(r3), "space name.txt") == 0);
    auto r4 = run(f.store, "mixed.case");
    CHECK(pos(names(r4), "MiXeD.Case") == 0);
    auto r5 = run(f.store, "%");
    CHECK(pos(names(r5), "percent%file.txt") >= 0);
    CHECK_EQ(r5.size(), size_t{1});
    auto r6 = run(f.store, "_");
    CHECK(pos(names(r6), "underscore_file_.txt") >= 0);
}

TEST(search_ranking_exact_prefix_substring) {
    Fixture f;
    auto r = run(f.store, "foo");
    auto n = names(r);
    CHECK(r.size() >= 3);
    int exact = pos(n, "foo"), prefix = pos(n, "foo_bar"), contains = pos(n, "xfoo");
    CHECK(exact >= 0 && prefix >= 0 && contains >= 0);
    CHECK(exact < prefix);
    CHECK(prefix < contains);
    CHECK(pos(n, "dir_with_foo") < contains);  // start of a word beats the middle
    auto ra = run(f.store, "a");
    auto na = names(ra);
    CHECK(pos(na, "ab") >= 0 && pos(na, "ab") < pos(na, "abc"));
}

TEST(search_words_match_folders_too) {
    Fixture f;
    auto r = run(f.store, "nested inner");
    CHECK(pos(names(r), "inner.cpp") == 0);
    auto only = run(f.store, "nested");
    CHECK(pos(names(only), "inner.cpp") < 0);  // at least one word must be in the name
    auto q = run(f.store, "report quarterly");
    CHECK(pos(names(q), "Quarterly Report Final.pdf") == 0);
}

TEST(search_filters_and_operators) {
    Fixture f;
    auto neg = run(f.store, "foo -bar");
    CHECK(pos(names(neg), "foo_bar") < 0);
    CHECK(pos(names(neg), "foo") >= 0);
    auto txt = run(f.store, "*.txt");
    for (const auto& n : names(txt)) CHECK(n.size() > 4 && n.substr(n.size() - 4) == ".txt");
    CHECK_EQ(txt.size(), size_t{5});
    auto glob = run(f.store, "f*o");
    CHECK(pos(names(glob), "foo") >= 0);
    CHECK(pos(names(glob), "foo_bar") < 0);
    auto in = run(f.store, "in:nested");
    CHECK_EQ(in.size(), size_t{3});
    auto path = run(f.store, std::string("nested_dir") + fileio::kPathSep + "inn");
    CHECK(pos(names(path), "inner.cpp") == 0);
    CHECK_EQ(path.size(), size_t{1});
    auto folders = run(f.store, "foo", TypeFilter::Folders);
    CHECK_EQ(folders.size(), size_t{1});
    CHECK(pos(names(folders), "dir_with_foo") == 0);
    auto big = run(f.store, "size:>1mb");
    CHECK_EQ(big.size(), size_t{1});
    auto recent = run(f.store, "modified:week");
    CHECK(pos(names(recent), "cmake.txt") >= 0);
    CHECK(pos(names(recent), "foo") < 0);
    auto apps = run(f.store, "chrome", TypeFilter::Apps);
    CHECK(pos(names(apps), "Google Chrome.lnk") == 0);
}

TEST(search_fuzzy_fallback) {
    Fixture f;
    auto r = run(f.store, "qrtrly");
    CHECK(r.fuzzy);
    CHECK(pos(names(r), "Quarterly Report Final.pdf") == 0);
    auto none = run(f.store, "zzzzqqq");
    CHECK(none.empty());
}

TEST(search_scope_and_recent) {
    Fixture f;
    uint32_t nested = f.store->find_path(std::string("pearch_fixture") + fileio::kPathSep + "nested_dir");
    CHECK(nested != kNone);
    auto r = run(f.store, "txt", TypeFilter::All, nested);
    CHECK_EQ(r.size(), size_t{1});
    CHECK(pos(names(r), "cmake.txt") == 0);
    auto recent = run(f.store, "");
    CHECK(recent.recent);
    CHECK(pos(names(recent), "cmake.txt") == 0);  // most recently modified first
    auto all_apps = run(f.store, "", TypeFilter::Apps);
    CHECK_EQ(all_apps.size(), size_t{1});
}

TEST(search_history_boost) {
    Fixture f;
    auto b = std::make_shared<Boosts>();
    uint32_t abc = f.store->find_path(std::string("pearch_fixture") + fileio::kPathSep + "abc");
    CHECK(abc != kNone);
    b->by_id[abc] = 400;
    b->ranked.push_back(abc);
    auto r = run(f.store, "a", TypeFilter::All, kNone, b);
    auto n = names(r);
    CHECK(pos(n, "abc") < pos(n, "ab"));
    auto recent = run(f.store, "", TypeFilter::All, kNone, b);
    CHECK(pos(names(recent), "abc") == 0);
}

TEST(search_incremental_matches_full_search) {
    // A big random index exercises the parallel paths too.
    std::mt19937 rng(42);
    const char* words[] = {"report", "invoice", "photo", "budget", "draft", "final", "notes", "q3",
                           "2024", "café", "Résumé", "summary", "backup", "project", "pearch", "data"};
    const char* exts[] = {".pdf", ".docx", ".txt", ".jpg", ".xlsx", ".cpp", ""};
    IndexBuilder b;
    uint32_t root = b.add_root("bigroot", kNow);
    std::vector<uint32_t> dirs{root};
    std::vector<uint32_t> stack{root};
    for (int i = 0; i < 120000; ++i) {
        std::string name = std::string(words[rng() % 16]) + (rng() % 2 ? " " : "_") + words[rng() % 16];
        if (rng() % 3 == 0) name += std::to_string(rng() % 100);
        bool dir = rng() % 8 == 0;
        if (!dir) name += exts[rng() % 7];
        // keep depth-first order: attach to the current folder or one of its parents
        while (stack.size() > 1 && rng() % 6 == 0) stack.pop_back();
        uint32_t id = b.add(stack.back(), name, dir ? kDir : 0, rng() % 100000, kNow - (rng() % 1000) * kDay);
        if (dir && stack.size() < 12) stack.push_back(id);
    }
    auto store = b.finish(kNow);
    CHECK(store != nullptr);
    ThreadPool pool;
    Searcher typing(pool);
    std::string q;
    for (char c : std::string("report fin")) {
        q.push_back(c);
        SearchRequest req;
        req.text = q;
        req.now_ms = kNow;
        auto inc = typing.search(store, req);
        Searcher fresh(pool);
        auto full = fresh.search(store, req);
        std::set<uint32_t> a, bset;
        for (size_t k = 0; k < inc.size(); ++k) a.insert(inc.id(k));
        for (size_t k = 0; k < full.size(); ++k) bset.insert(full.id(k));
        CHECK(a == bset);
        // Brute force for single words.
        if (q.find(' ') == std::string::npos) {
            std::set<uint32_t> brute;
            std::string fq = text::fold(q);
            for (uint32_t i = 0; i < store->size(); ++i)
                if (store->folded(i).find(fq) != std::string_view::npos) brute.insert(i);
            CHECK(brute == bset || full.fuzzy);
        }
        // Top results come out in rank order.
        for (size_t k = 1; k < std::min<size_t>(full.size(), 200); ++k) CHECK(full.score(k - 1) >= full.score(k));
    }
}
