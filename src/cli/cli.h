#pragma once

#include <optional>
#include <string>

struct CLIResult {
    bool showHelp = false;
    bool showVersion = false;
    bool doScan = false;
    bool verbose = false;
    std::optional<std::string> query;
    std::optional<std::string> scanPath;
    bool doIndex = false;
    std::optional<std::string> indexPath;
    bool doStats = false;
    size_t limit = 50;
    bool noColor = false;
    bool quiet = false;
};

class CLI {
public:
    static CLIResult parse(int argc, char** argv) noexcept;
};
