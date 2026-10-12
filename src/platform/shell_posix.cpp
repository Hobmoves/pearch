#include "platform/shell.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <climits>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "core/fileio.h"

extern char** environ;

namespace shell {

namespace {

// Runs a program detached from the terminal, discarding its output.
bool spawn_detached(const std::vector<std::string>& argv) {
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int rc = posix_spawnp(&pid, args[0], &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    return rc == 0;
}

bool pipe_to(const char* cmd, const std::string& data) {
    FILE* p = ::popen(cmd, "w");
    if (!p) return false;
    std::fwrite(data.data(), 1, data.size(), p);
    return ::pclose(p) == 0;
}

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return v && *v ? std::string(v) : fallback;
}

}  // namespace

void init() {}

std::vector<std::string> utf8_args(int argc, char** argv) {
    return std::vector<std::string>(argv, argv + argc);
}

bool open(const std::string& path) {
#ifdef __APPLE__
    return spawn_detached({"open", path});
#else
    return spawn_detached({"xdg-open", path});
#endif
}

bool reveal(const std::string& path) {
#ifdef __APPLE__
    return spawn_detached({"open", "-R", path});
#else
    return spawn_detached({"xdg-open", fileio::is_directory(path) ? path : fileio::parent_of(path)});
#endif
}

bool copy_text(const std::string& text) {
#ifdef __APPLE__
    return pipe_to("pbcopy 2>/dev/null", text);
#else
    if (std::getenv("WAYLAND_DISPLAY") && pipe_to("wl-copy 2>/dev/null", text)) return true;
    if (std::getenv("DISPLAY") && pipe_to("xclip -selection clipboard 2>/dev/null", text)) return true;
    return false;
#endif
}

std::string home_dir() { return env_or("HOME", "/"); }

std::string data_dir() {
    if (const char* v = std::getenv("PEARCH_HOME"); v && *v) return v;
    std::string base = env_or("XDG_DATA_HOME", fileio::join(home_dir(), ".local/share"));
    return fileio::join(base, "pearch");
}

std::string exe_path() {
#ifdef __APPLE__
    char mac[PATH_MAX];
    uint32_t size = sizeof mac;
    if (_NSGetExecutablePath(mac, &size) == 0) return mac;
#endif
    char buf[PATH_MAX];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) return std::string(buf, static_cast<size_t>(n));
    return "pearch";
}

Defaults default_locations() {
    Defaults d;
    std::string home = home_dir();
    d.roots.push_back(home);
    d.skip_paths.push_back(fileio::join(home, ".cache"));
    d.skip_paths.push_back(fileio::join(home, ".local/share/Trash"));
    for (const char* apps : {"/usr/share/applications", "/Applications"}) {
        if (fileio::is_directory(apps)) {
            d.roots.push_back(apps);
            d.app_paths.push_back(apps);
        }
    }
    return d;
}

std::vector<Step> install() {
    return {{true, "Nothing to install here: put the pearch binary somewhere on your PATH "
                   "(for example ~/.local/bin)."}};
}

std::vector<Step> uninstall(bool purge) {
    std::vector<Step> steps;
    if (purge) {
        std::error_code ec;
        std::filesystem::remove_all(data_dir(), ec);
        steps.push_back({!ec, !ec ? "Deleted the index and settings" : "Could not delete " + data_dir()});
    } else {
        steps.push_back({true, "Nothing to uninstall; use --purge to delete the index and settings."});
    }
    return steps;
}

}  // namespace shell
