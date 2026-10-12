#include "engine/query.h"

#include <cctype>
#include <cstdlib>
#include <ctime>

#include "core/fileio.h"
#include "core/text.h"

namespace engine {

namespace {

struct Token {
    std::string text;
    bool quoted = false;      // any part was quoted -> literal
    bool lead_quoted = false; // first character came from inside quotes
};

std::vector<Token> tokenize(std::string_view in) {
    std::vector<Token> out;
    Token cur;
    bool in_quote = false, any = false;
    for (char c : in) {
        if (static_cast<unsigned char>(c) < 0x20 && c != '\t') continue;  // strip controls
        if (c == '"') {
            in_quote = !in_quote;
            if (!any && in_quote) cur.lead_quoted = true;
            cur.quoted = true;
            any = true;
            continue;
        }
        if (!in_quote && (c == ' ' || c == '\t')) {
            if (any) out.push_back(std::move(cur));
            cur = Token{};
            any = false;
            continue;
        }
        cur.text.push_back(c);
        any = true;
    }
    if (any) out.push_back(std::move(cur));
    return out;
}

int64_t start_of_day(int64_t now_ms, int days_back) {
    std::time_t t = static_cast<std::time_t>(now_ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_mday -= days_back;
    tm.tm_isdst = -1;
    return static_cast<int64_t>(std::mktime(&tm)) * 1000;
}

bool parse_modified(std::string_view v, int64_t now_ms, int64_t& out) {
    std::string s = text::to_lower_ascii(v);
    while (!s.empty() && (s[0] == '>' || s[0] == '=')) s.erase(0, 1);
    constexpr int64_t kDay = 86400000LL;
    if (s == "today") { out = start_of_day(now_ms, 0); return true; }
    if (s == "yesterday") { out = start_of_day(now_ms, 1); return true; }
    if (s == "week" || s == "thisweek") { out = now_ms - 7 * kDay; return true; }
    if (s == "month" || s == "thismonth") { out = now_ms - 30 * kDay; return true; }
    if (s == "year" || s == "thisyear") { out = now_ms - 365 * kDay; return true; }
    if (s.size() >= 2) {
        char unit = s.back();
        char* end = nullptr;
        double n = std::strtod(s.c_str(), &end);
        if (end == s.c_str() + s.size() - 1 && n >= 0) {
            double mult = 0;
            switch (unit) {
                case 'h': mult = 3600000.0; break;
                case 'd': mult = static_cast<double>(kDay); break;
                case 'w': mult = 7.0 * kDay; break;
                case 'm': mult = 30.0 * kDay; break;
                case 'y': mult = 365.0 * kDay; break;
                default: return false;
            }
            out = now_ms - static_cast<int64_t>(n * mult);
            return true;
        }
    }
    return false;
}

void parse_size_filter(std::string_view v, Query& q) {
    std::string s = text::to_lower_ascii(text::trim(v));
    if (s == "empty") { q.size_max = 0; return; }
    if (s == "tiny") { q.size_max = 16 * 1024; return; }
    if (s == "small") { q.size_max = 1024 * 1024; return; }
    if (s == "medium") { q.size_min = 1024 * 1024; q.size_max = 128ull * 1024 * 1024; return; }
    if (s == "large") { q.size_min = 128ull * 1024 * 1024; return; }
    if (s == "huge") { q.size_min = 1024ull * 1024 * 1024; return; }
    bool less = false;
    size_t i = 0;
    if (i < s.size() && (s[i] == '<' || s[i] == '>')) { less = s[i] == '<'; ++i; }
    if (i < s.size() && s[i] == '=') ++i;
    uint64_t n = 0;
    if (!parse_size(std::string_view(s).substr(i), n)) return;
    if (less) q.size_max = n;
    else q.size_min = n;
}

}  // namespace

bool parse_size(std::string_view s, uint64_t& out) noexcept {
    std::string str(text::trim(s));
    if (str.empty()) return false;
    char* end = nullptr;
    double v = std::strtod(str.c_str(), &end);
    if (end == str.c_str() || v < 0) return false;
    std::string unit = text::to_lower_ascii(text::trim(std::string_view(end)));
    double mult = 1;
    if (unit.empty() || unit == "b") mult = 1;
    else if (unit == "k" || unit == "kb" || unit == "kib") mult = 1024.0;
    else if (unit == "m" || unit == "mb" || unit == "mib") mult = 1024.0 * 1024;
    else if (unit == "g" || unit == "gb" || unit == "gib") mult = 1024.0 * 1024 * 1024;
    else if (unit == "t" || unit == "tb" || unit == "tib") mult = 1024.0 * 1024 * 1024 * 1024;
    else return false;
    out = static_cast<uint64_t>(v * mult);
    return true;
}

Query parse_query(std::string_view input, int64_t now_ms) {
    Query q;
    q.raw = std::string(input);
    for (Token& tok : tokenize(input)) {
        std::string_view t = tok.text;
        bool negate = false;
        if (!tok.lead_quoted && t.size() > 1 && t[0] == '-') {
            negate = true;
            t.remove_prefix(1);
        }
        if (t.empty()) continue;

        if (!tok.quoted) {
            size_t colon = t.find(':');
            if (colon != std::string_view::npos && colon > 0) {
                std::string key = text::to_lower_ascii(t.substr(0, colon));
                std::string_view value = t.substr(colon + 1);
                bool handled = true;
                if (key == "ext" || key == "extension") {
                    for (auto& e : text::split(value, ',')) {
                        std::string_view ev = text::trim(e);
                        while (!ev.empty() && (ev[0] == '*' || ev[0] == '.')) ev.remove_prefix(1);
                        if (!ev.empty()) q.exts.push_back(text::fold(ev));
                    }
                    value = {};
                } else if (key == "file" || key == "files") {
                    q.type = TypeFilter::Files;
                } else if (key == "folder" || key == "folders" || key == "dir" || key == "dirs") {
                    q.type = TypeFilter::Folders;
                } else if (key == "app" || key == "apps") {
                    q.type = TypeFilter::Apps;
                } else if (key == "in" || key == "under") {
                    if (!value.empty())
                        q.terms.push_back({Term::InFolder, negate, text::fold(value)});
                    value = {};
                } else if (key == "size") {
                    parse_size_filter(value, q);
                    value = {};
                } else if (key == "modified" || key == "mod" || key == "dm" || key == "date") {
                    int64_t after = 0;
                    if (parse_modified(value, now_ms, after)) q.modified_after = after;
                    value = {};
                } else {
                    handled = false;
                }
                if (handled) {
                    if (value.empty()) continue;
                    t = value;  // e.g. "file:report" -> type filter + "report"
                }
            }
        }

        std::string folded = text::fold(t);
        if (!tok.quoted && (folded.find('*') != std::string::npos || folded.find('?') != std::string::npos)) {
            // "*.pdf" is just an extension filter (much faster than a glob). It
            // matches by prefix so results don't vanish while you type "*.p".
            if (!negate && folded.size() >= 2 && folded[0] == '*' && folded[1] == '.' &&
                folded.find_first_of("*?/\\", 2) == std::string::npos) {
                if (folded.size() > 2) q.ext_prefixes.push_back(folded.substr(2));
                continue;
            }
            bool all_wild = folded.find_first_not_of("*?") == std::string::npos;
            if (all_wild && folded.find('?') == std::string::npos) continue;  // "*" matches all
            q.terms.push_back({Term::Wildcard, negate, std::move(folded)});
            continue;
        }
        if (!tok.quoted && (folded.find('\\') != std::string::npos || folded.find('/') != std::string::npos)) {
            for (char& c : folded)
                if (c == '/' || c == '\\') c = fileio::kPathSep;
            q.terms.push_back({Term::Path, negate, std::move(folded)});
            continue;
        }
        q.terms.push_back({Term::Substr, negate, std::move(folded)});
    }
    return q;
}

bool Query::has_positive_name_terms() const noexcept {
    for (const auto& t : terms)
        if (!t.negate && (t.kind == Term::Substr || t.kind == Term::Wildcard)) return true;
    return false;
}

bool Query::has_filters() const noexcept {
    return !exts.empty() || !ext_prefixes.empty() || type != TypeFilter::All || size_min || size_max ||
           modified_after;
}

bool Query::refines(const Query& prev) const noexcept {
    if (type != prev.type || exts != prev.exts || ext_prefixes != prev.ext_prefixes || size_min != prev.size_min ||
        size_max != prev.size_max || modified_after != prev.modified_after)
        return false;
    std::vector<const Term*> a_pos, b_pos, a_other, b_other;
    for (const auto& t : terms) (t.kind == Term::Substr && !t.negate ? a_pos : a_other).push_back(&t);
    for (const auto& t : prev.terms) (t.kind == Term::Substr && !t.negate ? b_pos : b_other).push_back(&t);
    if (a_pos.size() != b_pos.size() || a_other.size() != b_other.size()) return false;
    if (a_pos.empty()) return false;
    for (size_t i = 0; i < a_other.size(); ++i)
        if (!(*a_other[i] == *b_other[i])) return false;
    for (size_t i = 0; i < a_pos.size(); ++i)
        if (a_pos[i]->text.find(b_pos[i]->text) == std::string::npos) return false;
    return true;
}

}  // namespace engine
