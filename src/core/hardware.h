#pragma once

// What machine are we on? pearch sizes its thread pools, picks its SIMD code
// path and tunes the scanner per drive from this, so a 16-thread desktop with
// an NVMe drive is used to the full while a laptop with a spinning disk isn't
// overwhelmed.

#include <cstdint>
#include <string>
#include <vector>

namespace hw {

struct Info {
    unsigned logical_cores = 1;
    uint64_t ram_bytes = 0;
    bool avx2 = false;
    std::string cpu;  // brand string, e.g. "AMD Ryzen 7 7800X3D 8-Core Processor"
};

// Detected once, then cached.
const Info& info();

enum class Storage { Unknown, Network, Hdd, Ssd, Nvme };
Storage storage_for(const std::string& path);
const char* storage_name(Storage s);

// Threads for data-parallel searching (all logical cores, capped at 64).
unsigned search_threads();

struct ScanPlan {
    unsigned threads = 4;
    Storage storage = Storage::Unknown;  // the slowest kind among the roots
};
// Scanner concurrency for these roots: deep queues for NVMe, a handful for
// spinning disks (more would just make the heads seek).
ScanPlan scan_plan(const std::vector<std::string>& roots);

// Optional overrides (from config.ini; 0 = automatic).
void set_overrides(unsigned scan_threads, unsigned search_threads);

// "16 threads · 64 GB RAM · AVX2"
std::string summary();

}  // namespace hw
