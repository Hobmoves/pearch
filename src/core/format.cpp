#include "core/format.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace fmt {

std::string thousands(uint64_t v) {
    std::string digits = std::to_string(v);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    int lead = static_cast<int>(digits.size() % 3);
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && static_cast<int>(i) % 3 == lead) out.push_back(',');
        out.push_back(digits[i]);
    }
    return out;
}

static std::string one_decimal(double v, const char* suffix) {
    char buf[32];
    if (v < 10.0) std::snprintf(buf, sizeof buf, "%.1f%s", v, suffix);
    else std::snprintf(buf, sizeof buf, "%.0f%s", v, suffix);
    return buf;
}

std::string compact(uint64_t v) {
    if (v >= 1000000000ULL) return one_decimal(v / 1e9, "B");
    if (v >= 1000000ULL) return one_decimal(v / 1e6, "M");
    if (v >= 10000ULL) return one_decimal(v / 1e3, "K");
    return thousands(v);
}

std::string size(uint64_t bytes) {
    const char* units[] = {" KB", " MB", " GB", " TB", " PB"};
    if (bytes < 1024) return std::to_string(bytes) + " B";
    double v = static_cast<double>(bytes) / 1024.0;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    return one_decimal(v, units[u]);
}

std::string duration(double seconds) {
    char buf[48];
    if (seconds < 1.0) {
        std::snprintf(buf, sizeof buf, "%.0f ms", seconds * 1000.0);
    } else if (seconds < 60.0) {
        std::snprintf(buf, sizeof buf, "%.1f s", seconds);
    } else {
        int total = static_cast<int>(seconds + 0.5);
        std::snprintf(buf, sizeof buf, "%d min %d s", total / 60, total % 60);
    }
    return buf;
}

std::string query_time(double ms) {
    char buf[32];
    if (ms < 10.0) std::snprintf(buf, sizeof buf, "%.1f ms", ms);
    else std::snprintf(buf, sizeof buf, "%.0f ms", ms);
    return buf;
}

static bool local_tm(int64_t unix_ms, std::tm& out) {
    std::time_t t = static_cast<std::time_t>(unix_ms / 1000);
#ifdef _WIN32
    return localtime_s(&out, &t) == 0;
#else
    return localtime_r(&t, &out) != nullptr;
#endif
}

std::string relative_time(int64_t unix_ms, int64_t now_ms_v) {
    if (unix_ms <= 0) return "";
    int64_t diff = (now_ms_v - unix_ms) / 1000;
    if (diff < 0) diff = 0;
    char buf[32];
    if (diff < 60) return "just now";
    if (diff < 3600) { std::snprintf(buf, sizeof buf, "%d min ago", static_cast<int>(diff / 60)); return buf; }
    if (diff < 86400) { std::snprintf(buf, sizeof buf, "%d hr ago", static_cast<int>(diff / 3600)); return buf; }

    std::tm then{}, now{};
    if (!local_tm(unix_ms, then) || !local_tm(now_ms_v, now)) return "";
    // Calendar-day difference, so "yesterday" means yesterday.
    std::tm a = then, b = now;
    a.tm_hour = b.tm_hour = 12; a.tm_min = b.tm_min = 0; a.tm_sec = b.tm_sec = 0;
    long days = std::lround(std::difftime(std::mktime(&b), std::mktime(&a)) / 86400.0);
    if (days <= 1) return "yesterday";
    if (days < 7) { std::snprintf(buf, sizeof buf, "%ld days ago", days); return buf; }
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (then.tm_year == now.tm_year || days < 180) {
        std::snprintf(buf, sizeof buf, "%s %d", months[then.tm_mon], then.tm_mday);
    } else {
        std::snprintf(buf, sizeof buf, "%s %d", months[then.tm_mon], then.tm_year + 1900);
    }
    return buf;
}

std::string date_time(int64_t unix_ms) {
    std::tm t{};
    if (unix_ms <= 0 || !local_tm(unix_ms, t)) return "unknown";
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &t);
    return buf;
}

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace fmt
