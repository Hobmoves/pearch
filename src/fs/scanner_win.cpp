#include "fs/scanner.h"

#include <windows.h>

#include "core/text.h"

namespace scan::detail {

namespace {

// \\?\ prefix lifts the MAX_PATH limit (paths must be absolute with '\').
std::wstring long_path(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\", 0) == 0) return path;
    if (path.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC" + path.substr(1);  // \\server\share
    return L"\\\\?\\" + path;
}

int64_t filetime_to_unix_ms(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    if (u.QuadPart < 116444736000000000ULL) return 0;
    return static_cast<int64_t>((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

std::string narrow(const wchar_t* w) {
    int len = static_cast<int>(wcslen(w));
    if (len == 0) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, out.data(), size, nullptr, nullptr);
    return out;
}

}  // namespace

NativePath to_native(const std::string& utf8) { return text::to_wide(utf8); }

NativePath join(const NativePath& dir, const std::string& name_utf8) {
    NativePath out = dir;
    if (!out.empty() && out.back() != L'\\' && out.back() != L'/') out.push_back(L'\\');
    out += text::to_wide(name_utf8);
    return out;
}

bool list_dir(const NativePath& dir, std::vector<DirEntry>& out) {
    std::wstring pattern = long_path(dir);
    if (pattern.back() != L'\\') pattern.push_back(L'\\');
    pattern.push_back(L'*');

    WIN32_FIND_DATAW fd;
    // FindExInfoBasic skips the short 8.3 name; LARGE_FETCH reads entries in
    // big batches. Together they are ~2x faster than plain FindFirstFileW.
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        const wchar_t* n = fd.cFileName;
        if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
        DirEntry e;
        e.name = narrow(n);
        const DWORD a = fd.dwFileAttributes;
        e.is_dir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.hidden = (a & FILE_ATTRIBUTE_HIDDEN) != 0;
        e.system = (a & FILE_ATTRIBUTE_SYSTEM) != 0;
        // Only real links are skipped: OneDrive / cloud placeholders are also
        // reparse points but must be indexed.
        if (a & FILE_ATTRIBUTE_REPARSE_POINT)
            e.link = fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT || fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK;
        if (!e.is_dir) {
            ULARGE_INTEGER sz;
            sz.LowPart = fd.nFileSizeLow;
            sz.HighPart = fd.nFileSizeHigh;
            e.size = sz.QuadPart;
        }
        e.mtime = filetime_to_unix_ms(fd.ftLastWriteTime);
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

void lower_thread_priority() {
    // Background mode also lowers I/O and memory priority, so a refresh never
    // gets in the way of what you are doing.
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
}

bool stat_dir(const NativePath& path, int64_t& mtime) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(long_path(path).c_str(), GetFileExInfoStandard, &data)) return false;
    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    mtime = filetime_to_unix_ms(data.ftLastWriteTime);
    return true;
}

}  // namespace scan::detail
