#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace app {

enum class Command {
    Search, Index, Add, Remove, Locations, Stats, Install, Uninstall, Help, Version, Scan, PearDemo,
};

struct Options {
    Command cmd = Command::Search;
    std::vector<std::string> args;  // query words, or paths for add/remove/scan
    bool list = false;              // print results instead of opening the UI
    bool quiet = false;
    bool verbose = false;
    bool no_color = false;
    bool purge = false;
    size_t limit = 0;               // 0 = default
    std::optional<std::string> in_dir;
    std::string error;

    std::string query() const;      // args joined back into one query
};

// argv[0] is skipped. Unknown "-word" arguments are query terms (exclusions).
Options parse(const std::vector<std::string>& argv);

}  // namespace app
