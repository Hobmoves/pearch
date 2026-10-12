#pragma once

#include "filesystem/file_entry.h"
#include <string>
#include <functional>

namespace Scanner {

struct ScanResult {
    uint64_t files = 0;
    uint64_t directories = 0;
    uint64_t errors = 0;
    double elapsed_seconds = 0.0;
};

// Callback invoked for each discovered entry during scanning.
using EntryCallback = std::function<void(const FileEntry&)>;

// Single-threaded Windows-native filesystem scanner
// path should be an absolute Windows path (e.g. C:\Users)
// Two overloads are provided: a simple counter-based scan and a callback-based scan.
ScanResult scan(const std::wstring& path, bool verbose = false) noexcept;
ScanResult scan(const std::wstring& path, EntryCallback cb, bool verbose = false) noexcept;

} // namespace Scanner
