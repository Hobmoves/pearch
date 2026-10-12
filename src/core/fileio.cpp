#include "core/fileio.h"

#include <filesystem>
#include <system_error>

#include "core/text.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace fileio {

static fs::path native(const std::string& p) {
#ifdef _WIN32
    return fs::path(text::to_wide(p));
#else
    return fs::path(p);
#endif
}

static std::string from_native(const fs::path& p) {
#ifdef _WIN32
    return text::to_utf8(p.native());
#else
    return p.native();
#endif
}

FILE* open(const std::string& path, const char* mode) {
#ifdef _WIN32
    return _wfopen(text::to_wide(path).c_str(), text::to_wide(mode).c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

bool read_all(const std::string& path, std::string& out) {
    FILE* f = open(path, "rb");
    if (!f) return false;
    out.clear();
    char buf[1 << 16];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    bool ok = !std::ferror(f);
    std::fclose(f);
    return ok;
}

bool replace(const std::string& from, const std::string& to) {
#ifdef _WIN32
    return MoveFileExW(text::to_wide(from).c_str(), text::to_wide(to).c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

bool write_atomic(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    FILE* f = open(tmp, "wb");
    if (!f) return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (std::fflush(f) == 0) && ok;
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) { remove_file(tmp); return false; }
    return replace(tmp, path);
}

bool exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(native(path), ec);
}

bool is_directory(const std::string& path) {
    std::error_code ec;
    return fs::is_directory(native(path), ec);
}

bool make_dirs(const std::string& path) {
    std::error_code ec;
    fs::create_directories(native(path), ec);
    return is_directory(path);
}

bool remove_file(const std::string& path) {
    std::error_code ec;
    return fs::remove(native(path), ec);
}

uint64_t file_size(const std::string& path) {
    std::error_code ec;
    auto s = fs::file_size(native(path), ec);
    return ec ? 0 : static_cast<uint64_t>(s);
}

std::string absolute(const std::string& path) {
    std::error_code ec;
    fs::path p = native(path.empty() ? std::string(".") : path);
    fs::path abs = fs::absolute(p, ec);
    if (ec) abs = p;
    abs = abs.lexically_normal();
    std::string s = from_native(abs.make_preferred());
    // Strip trailing separators, but keep roots ("C:\", "/").
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) {
        if (s.size() == 3 && s[1] == ':') break;
        s.pop_back();
    }
    if (s.size() == 2 && s[1] == ':') s.push_back(kPathSep);
    return s;
}

std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + kPathSep + b;
}

std::string parent_of(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return {};
    if (pos == 0) return path.substr(0, 1);
    if (pos == 2 && path[1] == ':') return path.substr(0, 3);
    return path.substr(0, pos);
}

std::string basename_of(const std::string& path) {
    std::string p = path;
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    size_t pos = p.find_last_of("/\\");
    return pos == std::string::npos ? p : p.substr(pos + 1);
}

bool is_within(const std::string& path, const std::string& dir) {
#ifdef _WIN32
    auto eq = [](std::string_view a, std::string_view b) { return text::fold(a) == text::fold(b); };
#else
    auto eq = [](std::string_view a, std::string_view b) { return a == b; };
#endif
    if (path.size() < dir.size()) return false;
    if (!eq(std::string_view(path).substr(0, dir.size()), dir)) return false;
    if (path.size() == dir.size()) return true;
    char c = path[dir.size()];
    char last = dir.empty() ? 0 : dir.back();
    return c == '/' || c == '\\' || last == '/' || last == '\\';
}

}  // namespace fileio
