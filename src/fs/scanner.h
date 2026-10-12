#pragma once

// Parallel filesystem scanner. Worker threads pull folders from a shared
// stack, list them in large batches, and write into per-thread arenas; the
// arenas are stitched into an IndexStore (depth-first order) at the end.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "engine/index_store.h"

namespace scan {

struct Options {
    std::vector<std::string> roots;       // absolute paths to index
    std::vector<std::string> skip_names;  // folder names never descended (node_modules, .git, ...)
    std::vector<std::string> skip_paths;  // absolute folders never descended (AppData, ...)
    std::vector<std::string> app_paths;   // folders whose programs count as apps (Start Menu)
    unsigned threads = 0;                 // 0 = sized for the drive (see hw::scan_plan)
    bool background = false;              // low CPU + I/O priority (refresh while you type)
};

struct Progress {
    std::atomic<uint64_t> files{0};
    std::atomic<uint64_t> dirs{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<uint64_t> dirs_done{0};
    std::atomic<uint64_t> dirs_queued{0};
    std::atomic<bool> cancel{false};
    std::atomic<bool> finished{false};

    uint64_t entries() const noexcept { return files.load() + dirs.load(); }
    void set_current(const std::string& path);
    std::string current() const;

private:
    mutable std::mutex m_;
    std::string current_;
};

// Returns nullptr if cancelled.
std::shared_ptr<engine::IndexStore> build(const Options& opt, Progress& progress, int64_t now_ms);

// ---- platform layer (scanner_win.cpp / scanner_posix.cpp) ----
namespace detail {

#ifdef _WIN32
using NativePath = std::wstring;
#else
using NativePath = std::string;
#endif

struct DirEntry {
    std::string name;  // UTF-8
    bool is_dir = false;
    bool hidden = false;
    bool system = false;
    bool link = false;  // symlink / junction: never descended
    uint64_t size = 0;
    int64_t mtime = 0;  // unix ms
};

NativePath to_native(const std::string& utf8);
NativePath join(const NativePath& dir, const std::string& name_utf8);
// Lists a folder (without "." and ".."). Returns false if it can't be opened.
bool list_dir(const NativePath& dir, std::vector<DirEntry>& out);
// Is `path` an existing folder? Fills its modification time.
bool stat_dir(const NativePath& path, int64_t& mtime);
// Lowers the calling thread's CPU and disk priority.
void lower_thread_priority();

}  // namespace detail
}  // namespace scan
