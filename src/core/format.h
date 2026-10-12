#pragma once

// Human-friendly formatting of counts, sizes, times and durations.

#include <cstdint>
#include <string>

namespace fmt {

std::string thousands(uint64_t v);                 // 1284 -> "1,284"
std::string compact(uint64_t v);                   // 412331 -> "412K"
std::string size(uint64_t bytes);                  // "2.1 MB", "880 KB", "12 B"
std::string duration(double seconds);              // "850 ms", "3.2 s", "1 min 12 s"
std::string query_time(double ms);                 // "0.6 ms", "12 ms"
std::string relative_time(int64_t unix_ms, int64_t now_ms);  // "3 days ago", "Sep 12"
std::string date_time(int64_t unix_ms);            // "2026-09-12 14:03"

int64_t now_ms();

}  // namespace fmt
