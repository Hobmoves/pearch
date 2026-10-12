#pragma once

// Settings live in a small, hand-editable config.ini next to the index:
//   %LOCALAPPDATA%\pearch on Windows, ~/.local/share/pearch elsewhere,
//   or $PEARCH_HOME when set.

#include <string>
#include <vector>

#include "fs/scanner.h"

namespace app {

struct Paths {
    std::string dir, config, index, history;
    static Paths get();
};

struct Config {
    std::vector<std::string> locations;   // folders to index
    std::vector<std::string> app_paths;   // folders whose shortcuts are "apps"
    std::vector<std::string> skip_names;  // never look inside folders with these names
    std::vector<std::string> skip_paths;  // never look inside these folders
    int refresh_minutes = 10;             // refresh the index in the background when older
    unsigned scan_threads = 0;            // 0 = automatic (sized for your drives)
    unsigned search_threads = 0;          // 0 = automatic (all cores)

    static Config defaults();
    bool load(const std::string& file);
    bool save(const std::string& file) const;

    scan::Options scan_options() const;
    // A short label for a location ("~", "Start Menu", "D:\Work").
    std::string label(const std::string& location) const;
};

// Loads config.ini, creating it with defaults on first use.
Config load_or_create(const Paths& p);

}  // namespace app
