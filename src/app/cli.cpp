#include "app/cli.h"

#include <cstdlib>

namespace app {

std::string Options::query() const {
    std::string q;
    for (const auto& a : args) {
        if (!q.empty()) q += ' ';
        // An argument with spaces was quoted in the shell: keep it a phrase.
        if (a.find(' ') != std::string::npos && a.find('"') == std::string::npos) q += '"' + a + '"';
        else q += a;
    }
    return q;
}

Options parse(const std::vector<std::string>& argv) {
    Options o;
    bool command_seen = false;
    bool rest_is_query = false;
    for (size_t i = 1; i < argv.size(); ++i) {
        const std::string& a = argv[i];
        auto next = [&](std::string& out) {
            if (i + 1 < argv.size()) { out = argv[++i]; return true; }
            return false;
        };
        if (rest_is_query) { o.args.push_back(a); continue; }
        if (a == "--") { rest_is_query = true; continue; }
        if (a == "-h" || a == "--help" || a == "/?") { o.cmd = Command::Help; return o; }
        if (a == "-v" || a == "-V" || a == "--version") { o.cmd = Command::Version; return o; }
        if (a == "-l" || a == "--list" || a == "-p" || a == "--print") { o.list = true; continue; }
        if (a == "-q" || a == "--quiet") { o.quiet = true; continue; }
        if (a == "--verbose") { o.verbose = true; continue; }
        if (a == "--no-color" || a == "--no-colour") { o.no_color = true; continue; }
        if (a == "--purge") { o.purge = true; continue; }
        if (a == "--pear") { o.cmd = Command::PearDemo; command_seen = true; continue; }
        if (a == "-n" || a == "--limit") {
            std::string v;
            if (!next(v)) { o.error = a + " needs a number"; return o; }
            long n = std::strtol(v.c_str(), nullptr, 10);
            if (n <= 0) { o.error = "invalid limit: " + v; return o; }
            o.limit = static_cast<size_t>(n);
            continue;
        }
        if (a == "--in") {
            std::string v;
            if (!next(v)) { o.error = "--in needs a folder"; return o; }
            o.in_dir = v;
            continue;
        }
        // Legacy flags from pearch 0.1.
        if (a == "--stats") { o.cmd = Command::Stats; command_seen = true; continue; }
        if (a == "--install") { o.cmd = Command::Install; command_seen = true; continue; }
        if (a == "--uninstall") { o.cmd = Command::Uninstall; command_seen = true; continue; }
        if (a == "--index") {
            o.cmd = Command::Index;
            command_seen = true;
            if (i + 1 < argv.size() && argv[i + 1].rfind("-", 0) != 0) {
                o.cmd = Command::Add;
                o.args.push_back(argv[++i]);
            }
            continue;
        }
        if (a == "--scan") {
            o.cmd = Command::Scan;
            command_seen = true;
            std::string v;
            if (!next(v)) { o.error = "--scan needs a folder"; return o; }
            o.args.push_back(v);
            continue;
        }
        if (!command_seen && o.args.empty()) {
            command_seen = true;
            if (a == "search" || a == "find") continue;
            if (a == "index" || a == "reindex" || a == "refresh") { o.cmd = Command::Index; continue; }
            if (a == "add") { o.cmd = Command::Add; continue; }
            if (a == "remove" || a == "rm") { o.cmd = Command::Remove; continue; }
            if (a == "locations" || a == "where") { o.cmd = Command::Locations; continue; }
            if (a == "stats" || a == "status") { o.cmd = Command::Stats; continue; }
            if (a == "install") { o.cmd = Command::Install; continue; }
            if (a == "uninstall") { o.cmd = Command::Uninstall; continue; }
            if (a == "help") { o.cmd = Command::Help; return o; }
            if (a == "version") { o.cmd = Command::Version; return o; }
        }
        o.args.push_back(a);
    }
    if ((o.cmd == Command::Add || o.cmd == Command::Remove) && o.args.empty())
        o.error = std::string(o.cmd == Command::Add ? "add" : "remove") + " needs a folder, e.g. pearch " +
                  (o.cmd == Command::Add ? "add D:\\Projects" : "remove D:\\Projects");
    return o;
}

}  // namespace app
