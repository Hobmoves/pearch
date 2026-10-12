#pragma once

#include <string>
#include <windows.h>
#include <cstdint>

struct FileEntry {
    std::wstring full_path;
    std::wstring filename;
    std::wstring extension;
    bool is_directory = false;
    uint64_t file_size = 0;
    FILETIME last_write_time{};
};
