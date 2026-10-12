#include <iostream>
#include "cli/cli.h"
#include "ui/terminal.h"
#include <filesystem>
#include "index/index.h"

int main(int argc, char** argv) {
    Terminal::init();
    Terminal::printLogo();
    Terminal::printDescription();

    auto res = CLI::parse(argc, argv);
    if (res.showHelp) {
        Terminal::printHelp();
        return 0;
    }
    if (res.showVersion) {
        Terminal::printVersion();
        return 0;
    }
    if (res.doScan) {
        if (res.scanPath) {
            std::wstring wpath(res.scanPath->begin(), res.scanPath->end());
            auto r = Scanner::scan(wpath, res.verbose);
            Terminal::printScanSummary(*res.scanPath, r);
        } else {
            std::cout << "--scan requires a path argument\n";
            return 1;
        }
        return 0;
    }

    // continue to handle index/stats/search below

    // Indexing
    if (res.doIndex) {
        std::wstring target;
        if (res.indexPath) {
            target.assign(res.indexPath->begin(), res.indexPath->end());
        } else {
            // default: current directory
            target = std::filesystem::current_path().wstring();
        }
        Scanner::ScanResult scan_r;
        double elapsed = 0.0;
        bool ok = Index::build_index(target, res.verbose, scan_r, elapsed, res.noColor, res.quiet);
        if (!ok) {
            std::cout << "Indexing failed\n";
            return 1;
        }
        Terminal::printIndexSummary(res.indexPath ? *res.indexPath : std::string("."), scan_r, elapsed);
        return 0;
    }

    if (res.doStats) {
        auto s = Index::stats();
        Terminal::printStats(s);
        return 0;
    }

    // Actual search: pearch <query>
    if (res.query) {
        // normalize query: trim whitespace
        std::string q = *res.query;
        auto l = q.find_first_not_of(" \t\n\r");
        if (l == std::string::npos) {
            std::cout << "Empty query after trimming; please provide a search term.\n";
            return 1;
        }
        auto rpos = q.find_last_not_of(" \t\n\r");
        q = q.substr(l, rpos - l + 1);
        if (q.empty()) {
            std::cout << "Empty query; please provide a search term.\n";
            return 1;
        }
        double qms = 0.0;
        auto results = Index::search(q, res.limit, qms);
        Terminal::printSearchResults(q, results, qms);
        return 0;
    }

    return 0;
}
