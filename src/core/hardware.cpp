#include "core/hardware.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

#include "core/text.h"

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#else
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <fstream>
#endif

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#include <immintrin.h>
#define PEARCH_X86 1
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>
#define PEARCH_X86 1
#endif

namespace hw {

namespace {

std::atomic<unsigned> g_scan_override{0};
std::atomic<unsigned> g_search_override{0};

#ifdef PEARCH_X86
void cpuid(unsigned leaf, unsigned sub, unsigned out[4]) {
#if defined(_MSC_VER)
    int r[4];
    __cpuidex(r, static_cast<int>(leaf), static_cast<int>(sub));
    for (int i = 0; i < 4; ++i) out[i] = static_cast<unsigned>(r[i]);
#else
    __cpuid_count(leaf, sub, out[0], out[1], out[2], out[3]);
#endif
}

uint64_t xgetbv0() {
#if defined(_MSC_VER)
    return _xgetbv(0);
#else
    uint32_t eax, edx;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return (static_cast<uint64_t>(edx) << 32) | eax;
#endif
}

void detect_cpu(Info& info) {
    unsigned r[4];
    cpuid(0, 0, r);
    const unsigned max_leaf = r[0];
    if (max_leaf >= 7) {
        cpuid(1, 0, r);
        const bool osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1;
        unsigned r7[4];
        cpuid(7, 0, r7);
        const bool avx2 = (r7[1] >> 5) & 1;
        info.avx2 = osxsave && avx && avx2 && (xgetbv0() & 6) == 6;
    }
    cpuid(0x80000000u, 0, r);
    if (r[0] >= 0x80000004u) {
        char brand[49] = {};
        for (unsigned i = 0; i < 3; ++i) {
            cpuid(0x80000002u + i, 0, r);
            std::memcpy(brand + i * 16, r, 16);
        }
        info.cpu = std::string(text::trim(brand));
    }
}
#endif

Info detect() {
    Info info;
    info.logical_cores = std::max(1u, std::thread::hardware_concurrency());
#ifdef _WIN32
    DWORD all = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (all > info.logical_cores) info.logical_cores = all;
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) info.ram_bytes = ms.ullTotalPhys;
#else
    long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page > 0) info.ram_bytes = static_cast<uint64_t>(pages) * static_cast<uint64_t>(page);
#endif
#ifdef PEARCH_X86
    detect_cpu(info);
#endif
    if (const char* off = std::getenv("PEARCH_NO_AVX2"); off && *off) info.avx2 = false;
    return info;
}

int rank(Storage s) {
    switch (s) {
        case Storage::Hdd: return 0;
        case Storage::Network: return 1;
        case Storage::Unknown: return 2;
        case Storage::Ssd: return 3;
        case Storage::Nvme: return 4;
    }
    return 2;
}

}  // namespace

const Info& info() {
    static const Info cached = detect();
    return cached;
}

#ifdef _WIN32
Storage storage_for(const std::string& path) {
    std::wstring w = text::to_wide(path);
    if (w.rfind(L"\\\\", 0) == 0 && w.rfind(L"\\\\?\\", 0) != 0) return Storage::Network;
    wchar_t vol[MAX_PATH] = {};
    if (!GetVolumePathNameW(w.c_str(), vol, MAX_PATH)) return Storage::Unknown;
    UINT type = GetDriveTypeW(vol);
    if (type == DRIVE_REMOTE) return Storage::Network;
    if (!(vol[0] && vol[1] == L':')) return Storage::Unknown;
    std::wstring dev = L"\\\\.\\";
    dev += vol[0];
    dev += L':';
    // Zero access rights: enough for property queries, no admin needed.
    HANDLE h = CreateFileW(dev.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Storage::Unknown;
    Storage result = Storage::Unknown;
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    alignas(8) BYTE buf[1024];
    DWORD got = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf, sizeof buf, &got, nullptr) &&
        got >= sizeof(STORAGE_DEVICE_DESCRIPTOR) - 1) {
        auto* d = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf);
        if (static_cast<int>(d->BusType) == 17 /* BusTypeNvme */) result = Storage::Nvme;
    }
    if (result == Storage::Unknown) {
        q.PropertyId = StorageDeviceSeekPenaltyProperty;
        DEVICE_SEEK_PENALTY_DESCRIPTOR sp{};
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &sp, sizeof sp, &got, nullptr) &&
            got >= sizeof sp)
            result = sp.IncursSeekPenalty ? Storage::Hdd : Storage::Ssd;
    }
    CloseHandle(h);
    return result;
}
#else
Storage storage_for(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return Storage::Unknown;
    if (major(st.st_dev) == 0) return Storage::Unknown;  // tmpfs, overlay, network...
    std::string base = "/sys/dev/block/" + std::to_string(major(st.st_dev)) + ":" + std::to_string(minor(st.st_dev));
    char real[PATH_MAX];
    if (!::realpath(base.c_str(), real)) return Storage::Unknown;
    std::string dev = real;
    if (dev.find("/nvme") != std::string::npos) return Storage::Nvme;
    for (const std::string& q : {dev + "/queue/rotational", dev + "/../queue/rotational"}) {
        std::ifstream f(q);
        int rot = -1;
        if (f >> rot) return rot ? Storage::Hdd : Storage::Ssd;
    }
    return Storage::Unknown;
}
#endif

const char* storage_name(Storage s) {
    switch (s) {
        case Storage::Nvme: return "NVMe SSD";
        case Storage::Ssd: return "SSD";
        case Storage::Hdd: return "hard disk";
        case Storage::Network: return "network drive";
        case Storage::Unknown: return "disk";
    }
    return "disk";
}

void set_overrides(unsigned scan_threads, unsigned search_threads_v) {
    g_scan_override = std::min(scan_threads, 256u);
    g_search_override = std::min(search_threads_v, 256u);
}

unsigned search_threads() {
    if (unsigned o = g_search_override.load()) return o;
    return std::clamp(info().logical_cores, 1u, 64u);
}

ScanPlan scan_plan(const std::vector<std::string>& roots) {
    ScanPlan plan;
    const unsigned cores = info().logical_cores;
    Storage best = Storage::Hdd;
    bool any = false;
    for (const auto& r : roots) {
        Storage s = storage_for(r);
        if (!any || rank(s) > rank(best)) best = s;
        any = true;
    }
    plan.storage = any ? best : Storage::Unknown;
    switch (plan.storage) {
        // NVMe drives answer many requests at once: keep their queues full.
        case Storage::Nvme: plan.threads = std::clamp(cores * 2, 8u, 64u); break;
        case Storage::Ssd: plan.threads = std::clamp(cores, 4u, 32u); break;
        case Storage::Network: plan.threads = std::clamp(cores, 4u, 16u); break;
        case Storage::Unknown: plan.threads = std::clamp(cores, 4u, 16u); break;
        // A spinning disk has one head; more threads only add seeking.
        case Storage::Hdd: plan.threads = 3; break;
    }
    if (info().ram_bytes && info().ram_bytes < (4ull << 30)) plan.threads = std::min(plan.threads, 8u);
    if (unsigned o = g_scan_override.load()) plan.threads = o;
    return plan;
}

std::string summary() {
    const Info& i = info();
    std::string s = std::to_string(i.logical_cores) + " threads";
    if (i.ram_bytes) s += " · " + std::to_string((i.ram_bytes + (1ull << 29)) >> 30) + " GB RAM";
    s += i.avx2 ? " · AVX2" : " · SSE2";
    return s;
}

}  // namespace hw
