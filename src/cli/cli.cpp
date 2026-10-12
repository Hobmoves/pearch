#include "cli/cli.h"
#include <string_view>

CLIResult CLI::parse(int argc, char** argv) noexcept {
    CLIResult r;
    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        if (a == "--help" || a == "-h") {
            r.showHelp = true;
            return r;
        }
        if (a == "--version" || a == "-v") {
            r.showVersion = true;
            return r;
        }
        if (a == "--verbose") {
            r.verbose = true;
            continue;
        }
        if (a == "--no-color") {
            r.noColor = true;
            continue;
        }
        if (a == "--quiet") {
            r.quiet = true;
            continue;
        }
        if (a == "--scan") {
            r.doScan = true;
            if (i + 1 < argc) {
                r.scanPath = std::string(argv[i + 1]);
                ++i;
            }
            continue;
        }
        if (a == "--index") {
            r.doIndex = true;
            if (i + 1 < argc) {
                r.indexPath = std::string(argv[i + 1]);
                ++i;
            }
            continue;
        }
        if (a == "--limit") {
            if (i + 1 < argc) {
                try {
                    int v = std::stoi(argv[i + 1]);
                    if (v > 0) r.limit = (size_t)v;
                } catch (...) {}
                ++i;
            }
            continue;
        }
        if (a == "--stats") {
            r.doStats = true;
            return r;
        }
        // first non-flag argument is the query
        if (!a.empty() && a[0] != '-') {
            r.query = std::string(a);
            // don't break — allow later flags, but for now keep simple
        }
    }
    return r;
}
