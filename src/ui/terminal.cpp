#include "ui/terminal.h"
#include "core/version.h"
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#endif
#include "filesystem/scanner.h"
#include "index/index.h"

namespace Terminal {

void init() noexcept {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#if defined(_WIN32)
    // enable VT processing on Windows consoles
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD dwMode = 0;
        if (GetConsoleMode(hOut, &dwMode)) {
            dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, dwMode);
        }
    }
#endif
#endif
}

void printLogo() noexcept {
    // ASCII/Unicode logo
    std::cout << R"(

██████╗ ███████╗ █████╗ ██████╗  ██████╗██╗  ██╗
██╔══██╗██╔════╝██╔══██╗██╔══██╗██╔════╝██║  ██║
██████╔╝█████╗  ███████║██████╔╝██║     ███████║
██╔═══╝ ██╔══╝  ██╔══██║██╔══██╗██║     ██╔══██║
██║     ███████╗██║  ██║██║  ██║╚██████╗██║  ██║
╚═╝     ╚══════╝╚═╝  ╚═╝╚═╝  ╚═╝ ╚═════╝╚═╝  ╚═╝

)";
}

void printDescription() noexcept {
    std::cout << "Fast, minimal Windows file search — pearch " << PEARCH_VERSION << "\n\n";
}

void printHelp() noexcept {
    std::cout << "Usage:\n";
    std::cout << "  pearch [query]          Search for files and folders (not implemented yet)\n";
    std::cout << "  pearch --help           Show this help\n";
    std::cout << "  pearch --version        Show version\n";
}

void printVersion() noexcept {
    std::cout << "pearch " << PEARCH_VERSION << "\n";
}

void printNotImplemented(const std::string& query) noexcept {
    std::cout << "Searching for: \"" << query << "\"\n";
    std::cout << "(Search engine not implemented yet — this is a placeholder.)\n";
}

void printUsageHint() noexcept {
    std::cout << "Type 'pearch --help' for usage information.\n";
}

void printScanSummary(const std::string& path, const Scanner::ScanResult& r) noexcept {
    std::cout << "\nScanning " << path << "...\n\n";
    std::cout << "Files: " << r.files << "\n";
    std::cout << "Directories: " << r.directories << "\n";
    std::cout << "Errors: " << r.errors << "\n";
    std::cout << "Time: " << r.elapsed_seconds << "s\n";
}

void printIndexSummary(const std::string& path, const Scanner::ScanResult& scan_r, double elapsed_seconds) noexcept {
    std::cout << "\nIndexing " << path << "...\n\n";
    std::cout << "Files: " << scan_r.files << "\n";
    std::cout << "Directories: " << scan_r.directories << "\n";
    std::cout << "Errors: " << scan_r.errors << "\n\n";
    uint64_t total = scan_r.files + scan_r.directories;
    double rate = total / (elapsed_seconds > 0 ? elapsed_seconds : 1.0);
    std::cout << "Entries: " << total << "  (" << rate << " entries/s)\n";
    std::cout << "Time: " << elapsed_seconds << "s\n";
}

void printSearchResults(const std::string& query, const std::vector<SearchResult>& results, double query_ms) noexcept {
    std::cout << "Search: " << query << "\n\n";
    std::cout << results.size() << " results · " << query_ms << " ms\n\n";
    int idx = 1;
    for (const auto& r : results) {
        std::cout << " " << idx << "  " << r.filename << "\n";
        std::cout << "    " << r.path << "\n\n";
        ++idx;
    }
}

void printStats(const IndexStats& s) noexcept {
    std::cout << "Indexed entries: " << s.entries << "\n";
    std::cout << "Files: " << s.files << "\n";
    std::cout << "Directories: " << s.directories << "\n";
    std::cout << "Database size: " << s.db_size_bytes << " bytes\n";
    std::cout << "Last indexed: " << s.last_indexed << "\n";
}

} // namespace Terminal

bool Terminal::supportsANSI() noexcept {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return false;
    DWORD dwMode = 0;
    if (!GetConsoleMode(hOut, &dwMode)) return false;
    return (dwMode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
    return true;
#endif
}
