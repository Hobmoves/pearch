// pearch_bench: builds a synthetic index and measures what you feel when
// typing: the latency of every keystroke.
//
//   pearch_bench [-n ENTRIES] [--dump paths.tsv] [--save index.bin]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "core/fileio.h"
#include "core/format.h"
#include "engine/searcher.h"

using namespace engine;
using Clock = std::chrono::steady_clock;

static double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

static std::shared_ptr<IndexStore> synthesize(size_t n) {
    static const char* words[] = {
        "report", "invoice", "photo", "budget", "draft", "final", "notes", "meeting", "summary", "backup",
        "project", "design", "screenshot", "resume", "contract", "presentation", "data", "export", "image",
        "video", "holiday", "family", "taxes", "receipt", "letter", "plan", "roadmap", "release", "build",
        "test", "config", "readme", "license", "index", "main", "utils", "server", "client", "assets",
        "q1", "q2", "q3", "q4", "2021", "2022", "2023", "2024", "2025", "v2", "old", "new", "copy"};
    static const char* exts[] = {".pdf", ".docx", ".xlsx", ".pptx", ".txt", ".md", ".jpg", ".png", ".mp4",
                                 ".zip", ".cpp", ".h", ".py", ".js", ".json", ".html", ".css", ".exe", ".lnk", ""};
    std::mt19937 rng(7);
    const size_t nw = sizeof words / sizeof *words, ne = sizeof exts / sizeof *exts;
    IndexBuilder b;
    b.reserve(n, n * 22);
    const int64_t now = fmt::now_ms();
    uint32_t root = b.add_root(std::string("C:") + fileio::kPathSep + "Users" + fileio::kPathSep + "bench", now);
    std::vector<uint32_t> stack{root};
    for (size_t i = 1; i < n; ++i) {
        std::string name = words[rng() % nw];
        int parts = 1 + static_cast<int>(rng() % 3);
        for (int k = 0; k < parts; ++k) {
            static const char seps[] = {' ', '_', '-'};
            name += seps[rng() % 3];
            name += words[rng() % nw];
        }
        if (rng() % 4 == 0) name += " (" + std::to_string(rng() % 20) + ")";
        const bool dir = rng() % 9 == 0;
        if (!dir) name += exts[rng() % ne];
        while (stack.size() > 1 && rng() % 5 == 0) stack.pop_back();
        uint32_t id = b.add(stack.back(), name, dir ? kDir : 0, rng() % 50000000, now - static_cast<int64_t>(rng() % 2000) * 86400000LL);
        if (dir && stack.size() < 14) stack.push_back(id);
    }
    return b.finish(now);
}

static double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t k = std::min(v.size() - 1, static_cast<size_t>(p * (v.size() - 1) + 0.5));
    return v[k];
}

int main(int argc, char** argv) {
    size_t n = 1000000;
    std::string dump, keep;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-n") && i + 1 < argc) n = static_cast<size_t>(std::atol(argv[++i]));
        else if (!std::strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else if (!std::strcmp(argv[i], "--save") && i + 1 < argc) keep = argv[++i];
    }

    std::printf("pearch benchmark  ·  %s entries\n\n", fmt::thousands(n).c_str());
    auto t0 = Clock::now();
    auto store = synthesize(n);
    std::printf("  build index in memory   %8.1f ms   (%s RAM)\n", ms_since(t0), fmt::size(store->memory_bytes()).c_str());

    std::string file = (std::filesystem::temp_directory_path() / "pearch_bench_index.bin").string();
    t0 = Clock::now();
    store->save(file);
    std::printf("  save snapshot           %8.1f ms   (%s on disk)\n", ms_since(t0), fmt::size(fileio::file_size(file)).c_str());
    t0 = Clock::now();
    auto loaded = IndexStore::load(file);
    std::printf("  load snapshot           %8.1f ms\n\n", ms_since(t0));
    if (!keep.empty()) fileio::replace(file, keep);
    else fileio::remove_file(file);
    if (!loaded) return 1;

    if (!dump.empty()) {
        FILE* f = fileio::open(dump, "wb");
        for (uint32_t i = 0; i < loaded->size(); ++i)
            std::fprintf(f, "%s\t%s\t%d\n", loaded->path(i).c_str(), std::string(loaded->name(i)).c_str(), loaded->is_dir(i) ? 1 : 0);
        std::fclose(f);
        std::printf("  dumped paths to %s\n\n", dump.c_str());
    }

    ThreadPool pool;
    const char* queries[] = {"report", "q3 budget", "invoice 2024", "screenshot", "*.pdf", "ext:xlsx taxes",
                             "rprt", "readme", "holiday photo", "main.cpp"};
    std::vector<double> keystrokes, fresh;
    std::printf("  %-18s %10s %12s %12s\n", "typed query", "results", "last key", "cold search");
    for (const char* q : queries) {
        Searcher typing(pool);
        std::string typed;
        double last = 0;
        size_t results = 0;
        for (const char* c = q; *c; ++c) {
            typed.push_back(*c);
            SearchRequest req;
            req.text = typed;
            req.now_ms = fmt::now_ms();
            auto start = Clock::now();
            auto r = typing.search(loaded, req);
            for (size_t k = 0; k < std::min<size_t>(r.size(), 40); ++k) (void)r.id(k);  // first screen
            last = ms_since(start);
            keystrokes.push_back(last);
            results = r.size();
        }
        Searcher cold(pool);
        SearchRequest req;
        req.text = q;
        req.now_ms = fmt::now_ms();
        auto start = Clock::now();
        auto r = cold.search(loaded, req);
        for (size_t k = 0; k < std::min<size_t>(r.size(), 40); ++k) (void)r.id(k);
        double c = ms_since(start);
        fresh.push_back(c);
        std::printf("  %-18s %10s %9.2f ms %9.2f ms\n", q, fmt::thousands(results).c_str(), last, c);
    }
    std::printf("\n  per keystroke   median %.2f ms   p95 %.2f ms   max %.2f ms   (%zu keystrokes, %u threads)\n",
                percentile(keystrokes, 0.5), percentile(keystrokes, 0.95), percentile(keystrokes, 1.0),
                keystrokes.size(), pool.size());
    std::printf("  cold search     median %.2f ms   max %.2f ms\n", percentile(fresh, 0.5), percentile(fresh, 1.0));
    return 0;
}
