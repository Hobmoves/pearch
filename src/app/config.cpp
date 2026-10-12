#include "app/config.h"

#include <algorithm>
#include <cstdlib>

#include "core/fileio.h"
#include "core/text.h"
#include "platform/shell.h"

namespace app {

Paths Paths::get() {
    Paths p;
    p.dir = shell::data_dir();
    fileio::make_dirs(p.dir);
    p.config = fileio::join(p.dir, "config.ini");
    p.index = fileio::join(p.dir, "index.bin");
    p.history = fileio::join(p.dir, "history.tsv");
    return p;
}

Config Config::defaults() {
    Config c;
    shell::Defaults d = shell::default_locations();
    c.locations = d.roots;
    c.app_paths = d.app_paths;
    c.skip_paths = d.skip_paths;
    c.skip_names = {"node_modules", ".git", ".svn", ".hg", "__pycache__", ".venv", ".tox",
                    ".gradle", "$Recycle.Bin", "System Volume Information"};
    return c;
}

bool Config::load(const std::string& file) {
    std::string data;
    if (!fileio::read_all(file, data)) return false;
    Config c;
    std::string section;
    for (const auto& raw : text::split(data, '\n')) {
        std::string_view line = text::trim(raw);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = text::to_lower_ascii(line.substr(1, line.size() - 2));
            continue;
        }
        std::string value(line);
        if (section == "locations") c.locations.push_back(value);
        else if (section == "apps") c.app_paths.push_back(value);
        else if (section == "skip") c.skip_names.push_back(value);
        else if (section == "skip-paths") c.skip_paths.push_back(value);
        else if (section == "options") {
            size_t eq = value.find('=');
            if (eq == std::string::npos) continue;
            std::string key = text::to_lower_ascii(text::trim(std::string_view(value).substr(0, eq)));
            std::string val(text::trim(std::string_view(value).substr(eq + 1)));
            if (key == "refresh-minutes") c.refresh_minutes = std::atoi(val.c_str());
            if (key == "scan-threads") c.scan_threads = static_cast<unsigned>(std::max(0, std::atoi(val.c_str())));
            if (key == "search-threads") c.search_threads = static_cast<unsigned>(std::max(0, std::atoi(val.c_str())));
        }
    }
    *this = std::move(c);
    return true;
}

bool Config::save(const std::string& file) const {
    std::string out =
        "# pearch settings. Edit freely, then run `pearch index`.\n"
        "# Lines starting with # are comments.\n\n"
        "# Folders to index (also: pearch add <folder> / pearch remove <folder>)\n[locations]\n";
    for (const auto& l : locations) out += l + "\n";
    out += "\n# Shortcuts and programs in these folders show up under Apps\n[apps]\n";
    for (const auto& l : app_paths) out += l + "\n";
    out += "\n# Folder names pearch never looks inside (the folder itself is still found)\n[skip]\n";
    for (const auto& l : skip_names) out += l + "\n";
    out += "\n# Folders pearch never looks inside\n[skip-paths]\n";
    for (const auto& l : skip_paths) out += l + "\n";
    out += "\n[options]\n# Refresh the index in the background when it is older than this\nrefresh-minutes = " +
           std::to_string(refresh_minutes) + "\n" +
           "# Threads: auto adapts to your CPU and drives (NVMe gets the most). Or set a number.\n"
           "scan-threads = " + (scan_threads ? std::to_string(scan_threads) : std::string("auto")) + "\n" +
           "search-threads = " + (search_threads ? std::to_string(search_threads) : std::string("auto")) + "\n";
    return fileio::write_atomic(file, out);
}

scan::Options Config::scan_options() const {
    scan::Options o;
    o.roots = locations;
    o.skip_names = skip_names;
    o.skip_paths = skip_paths;
    o.app_paths = app_paths;
    return o;
}

std::string Config::label(const std::string& location) const {
    for (const auto& a : app_paths)
        if (text::fold(a) == text::fold(location)) return "Start Menu";
    std::string home = shell::home_dir();
    if (!home.empty() && fileio::is_within(location, home)) {
        std::string rest = location.substr(home.size());
        return rest.empty() ? "~ (" + home + ")" : "~" + rest;
    }
    return location;
}

Config load_or_create(const Paths& p) {
    Config c;
    if (c.load(p.config) && !c.locations.empty()) return c;
    c = Config::defaults();
    c.save(p.config);
    return c;
}

}  // namespace app
