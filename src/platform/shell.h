#pragma once

// Desktop integration: opening files, revealing them in the file manager,
// the clipboard, well-known folders, and installing pearch for the user.

#include <string>
#include <vector>

namespace shell {

// Process-wide setup (COM on Windows). Call once at startup.
void init();
// Command-line arguments as UTF-8 (Windows reads the UTF-16 command line).
std::vector<std::string> utf8_args(int argc, char** argv);

bool open(const std::string& path);
bool reveal(const std::string& path);   // show in Explorer / Finder, selected
bool copy_text(const std::string& text);

std::string data_dir();   // %LOCALAPPDATA%\pearch, ~/.local/share/pearch, or $PEARCH_HOME
std::string home_dir();
std::string exe_path();

struct Defaults {
    std::vector<std::string> roots;
    std::vector<std::string> skip_paths;
    std::vector<std::string> app_paths;
};
Defaults default_locations();

struct Step {
    bool ok;
    std::string text;
};
std::vector<Step> install();
std::vector<Step> uninstall(bool purge);

}  // namespace shell
