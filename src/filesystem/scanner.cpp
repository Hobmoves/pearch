#include "filesystem/scanner.h"
#include <windows.h>
#include <vector>
#include <string>
#include <stack>
#include <chrono>
#include <iostream>

namespace {

std::wstring join_path(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

// Normalize to an absolute path with \?\ prefix for long paths when needed.
std::wstring normalize_for_find(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\", 0) == 0) return path; // already prefixed
    if (path.rfind(L"\\\\", 0) == 0) {
        // UNC path: \\server\share -> \\?\UNC\server\share
        return L"\\\\?\\UNC" + path.substr(1);
    }
    return L"\\\\?\\" + path;
}

std::string utf8_from_wstring(const std::wstring& w) {
    if (w.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    if (size <= 0) return {};
    std::string out(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), size, NULL, NULL);
    return out;
}

} // namespace

namespace Scanner {

struct FindHandle {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~FindHandle() noexcept { if (h != INVALID_HANDLE_VALUE) FindClose(h); }
};

// Internal implementation used by both overloads. If cb is non-null, it's invoked for each entry.
static ScanResult scan_impl(const std::wstring& path, EntryCallback cb, bool verbose) noexcept {
    ScanResult result;
    auto start = std::chrono::steady_clock::now();

    std::vector<std::wstring> stack;
    stack.push_back(path);

    while (!stack.empty()) {
        std::wstring dir = std::move(stack.back());
        stack.pop_back();

        std::wstring searchPath = normalize_for_find(dir) + L"\\*";

        WIN32_FIND_DATAW fd;
        FindHandle fh;
        fh.h = FindFirstFileW(searchPath.c_str(), &fd);
        if (fh.h == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            // Skip and count error
            result.errors++;
            if (verbose) {
                std::cout << "[error] " << utf8_from_wstring(dir) << " (" << err << ")\n";
            }
            continue;
        }

        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;

            std::wstring full = join_path(dir, name);

            bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            bool is_reparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

            FileEntry e;
            e.full_path = full;
            e.filename = name;
            // extract extension
            size_t pos = name.find_last_of(L'.');
            if (pos != std::wstring::npos) e.extension = name.substr(pos);
            e.is_directory = is_dir;
            e.last_write_time = fd.ftLastWriteTime;
            if (!is_dir) {
                ULARGE_INTEGER sz{};
                sz.LowPart = fd.nFileSizeLow;
                sz.HighPart = fd.nFileSizeHigh;
                e.file_size = sz.QuadPart;
            }

            if (is_dir) {
                result.directories++;
                if (verbose) std::cout << "[dir]  " << utf8_from_wstring(full) << "\n";
                if (!is_reparse) {
                    stack.push_back(full);
                } else {
                    if (verbose) std::cout << "[skip reparse] " << utf8_from_wstring(full) << "\n";
                }
            } else {
                result.files++;
                if (verbose) std::cout << "[file] " << utf8_from_wstring(full) << "\n";
            }

            if (cb) cb(e);

        } while (FindNextFileW(fh.h, &fd));

        DWORD last = GetLastError();
        if (last != ERROR_NO_MORE_FILES) {
            // An unexpected error occurred during iteration
            result.errors++;
            if (verbose) std::cout << "[error] iteration failed in " << utf8_from_wstring(dir) << " (" << last << ")\n";
        }
    }

    auto end = std::chrono::steady_clock::now();
    result.elapsed_seconds = std::chrono::duration<double>(end - start).count();
    return result;
}

ScanResult scan(const std::wstring& path, EntryCallback cb, bool verbose) noexcept {
    return scan_impl(path, std::move(cb), verbose);
}

ScanResult scan(const std::wstring& path, bool verbose) noexcept {
    return scan_impl(path, nullptr, verbose);
}


} // namespace Scanner
