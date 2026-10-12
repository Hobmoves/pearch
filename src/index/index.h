#pragma once

#include "filesystem/file_entry.h"
#include "filesystem/scanner.h"
#include <string>
#include <vector>

struct SearchResult {
    std::string path;
    std::string filename;
    bool is_directory = false;
};

struct IndexStats {
    uint64_t entries = 0;
    uint64_t files = 0;
    uint64_t directories = 0;
    uint64_t db_size_bytes = 0;
    std::string last_indexed;
};

namespace Index {

bool initialize_database();

// Test helper: override the default DB path used by initialize_database().
void set_db_path_for_tests(const std::wstring& path);

// Build or update the index for the given path. Returns true on success and fills stats.
bool build_index(const std::wstring& path, bool verbose, Scanner::ScanResult& out_scan_result, double& elapsed_seconds, bool no_color = false, bool quiet = false) noexcept;

// Search the index for a query. Returns list of results and sets query_time_ms.
std::vector<SearchResult> search(const std::string& query, size_t limit, double& query_time_ms) noexcept;

IndexStats stats() noexcept;

} // namespace Index
