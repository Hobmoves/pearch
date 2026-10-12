#pragma once

// The indexing screen: the 3D pear filling up next to live statistics.

#include <memory>
#include <string>
#include <vector>

#include "engine/index_store.h"
#include "fs/scanner.h"
#include "platform/console.h"
#include "tui/screen.h"

namespace tui {

struct IndexJob {
    scan::Options options;
    uint64_t expected = 0;              // entries in the previous index (0 = unknown)
    std::string title = "Indexing your files";
    std::vector<std::string> labels;    // short location names for display
    std::string hardware_note;          // "32 threads · NVMe SSD"
};

struct IndexOutcome {
    std::shared_ptr<engine::IndexStore> store;
    bool cancelled = false;
    double seconds = 0;
    uint64_t errors = 0;
};

// What one frame shows (separate from the loop so it can be tested).
struct IndexFrame {
    double time = 0;          // seconds since start
    double progress = 0;      // 0..1
    double flash = 0;         // completion animation 0..1
    bool done = false;
    uint64_t entries = 0, files = 0, dirs = 0, errors = 0;
    double rate = 0;          // entries per second
    std::vector<double> rate_history;
    std::string current;      // folder being scanned
    std::string title, subtitle, footer;
};
void draw_index_frame(Canvas& c, const IndexFrame& f, bool color);

// Full-screen version. `linger` keeps the finished state on screen briefly.
IndexOutcome run_index_view(term::Terminal& t, term::ColorMode mode, const IndexJob& job,
                            const std::string& done_hint, double linger_seconds);
// Line-based version for pipes / --quiet (progress on stderr unless quiet).
IndexOutcome run_index_plain(const IndexJob& job, bool quiet, term::ColorMode mode);

// Hidden "pearch --pear": the animation on its own, with simulated progress.
void run_pear_demo(term::Terminal& t, term::ColorMode mode, double seconds);

}  // namespace tui
