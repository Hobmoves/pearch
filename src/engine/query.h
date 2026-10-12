#pragma once

// Query language (all case- and accent-insensitive):
//
//   q3 report          every word must match (name, or a parent folder name)
//   "q3 report"        exact phrase
//   *.pdf  rep?rt*     wildcards (whole name)
//   ext:pdf,docx       extension filter
//   file: folder: app: type filters (also files:, dir:, apps:)
//   in:work            inside a folder whose name contains "work"
//   work\q3            path match (any term containing \ or /)
//   -draft             exclude
//   size:>10mb         size filter (>, <, >=, <=, or a bare number = at least)
//   modified:week      today, yesterday, week, month, year, or 3d / 12h / 2w

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine {

enum class TypeFilter : uint8_t { All, Files, Folders, Apps };

struct Term {
    enum Kind : uint8_t { Substr, Wildcard, Path, InFolder };
    Kind kind = Substr;
    bool negate = false;
    std::string text;  // folded
    bool operator==(const Term&) const = default;
};

struct Query {
    std::string raw;
    std::vector<Term> terms;
    std::vector<std::string> exts;  // folded, no dot (ext:pdf, exact)
    std::vector<std::string> ext_prefixes;  // from *.pd while typing: extension starts with this
    TypeFilter type = TypeFilter::All;
    std::optional<uint64_t> size_min, size_max;
    std::optional<int64_t> modified_after;  // unix ms

    bool has_positive_name_terms() const noexcept;
    bool has_filters() const noexcept;
    bool empty() const noexcept { return terms.empty() && !has_filters(); }
    // True when every result of *this is guaranteed to be a result of `prev`,
    // so a search can re-filter prev's matches instead of the whole index.
    bool refines(const Query& prev) const noexcept;
};

Query parse_query(std::string_view input, int64_t now_ms);

// Parses "10mb", "1.5 GB", "512" (bytes). Returns false on bad input.
bool parse_size(std::string_view s, uint64_t& out) noexcept;

}  // namespace engine
