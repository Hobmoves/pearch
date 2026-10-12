#pragma once

#include <string>
#include "filesystem/scanner.h"
#include "index/index.h"
#include <vector>

namespace Terminal {
    void init() noexcept;
    void printLogo() noexcept;
    void printDescription() noexcept;
    void printHelp() noexcept;
    void printVersion() noexcept;
    void printNotImplemented(const std::string& query) noexcept;
    void printUsageHint() noexcept;
    void printScanSummary(const std::string& path, const Scanner::ScanResult& r) noexcept;
    void printIndexSummary(const std::string& path, const Scanner::ScanResult& scan_r, double elapsed_seconds) noexcept;
    void printSearchResults(const std::string& query, const std::vector<SearchResult>& results, double query_ms) noexcept;
    void printStats(const IndexStats& s) noexcept;
    bool supportsANSI() noexcept;
}
