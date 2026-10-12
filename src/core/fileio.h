#pragma once

// Small UTF-8-path file helpers (Windows paths are converted to UTF-16 so
// non-ASCII user names and folders work everywhere).

#include <cstdint>
#include <cstdio>
#include <string>

namespace fileio {

#ifdef _WIN32
constexpr char kPathSep = '\\';
#else
constexpr char kPathSep = '/';
#endif

FILE* open(const std::string& path, const char* mode);
bool read_all(const std::string& path, std::string& out);
// Writes to "<path>.tmp" then atomically replaces <path>.
bool write_atomic(const std::string& path, const std::string& data);
bool replace(const std::string& from, const std::string& to);
bool exists(const std::string& path);
bool is_directory(const std::string& path);
bool make_dirs(const std::string& path);
bool remove_file(const std::string& path);
uint64_t file_size(const std::string& path);
// Absolute, normalized path with native separators and no trailing separator
// (except for roots like "C:\" or "/").
std::string absolute(const std::string& path);
std::string join(const std::string& a, const std::string& b);
std::string parent_of(const std::string& path);
std::string basename_of(const std::string& path);
// True if `path` equals `dir` or lies inside it (case-insensitive on Windows).
bool is_within(const std::string& path, const std::string& dir);

}  // namespace fileio
