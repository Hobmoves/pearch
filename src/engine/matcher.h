#pragma once

// String matching primitives. Everything operates on folded (lowercase,
// accent-free) UTF-8.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace engine {

struct Query;

namespace match {

constexpr size_t npos = static_cast<size_t>(-1);

// Substring search. Uses SSE2 (compare first and last needle byte 16
// positions at a time, verify with memcmp) where available.
size_t find(const char* hay, size_t n, const char* needle, size_t m) noexcept;
inline size_t find(std::string_view hay, std::string_view needle) noexcept {
    return find(hay.data(), hay.size(), needle.data(), needle.size());
}
inline bool contains(std::string_view hay, std::string_view needle) noexcept {
    return find(hay, needle) != npos;
}

// True when the AVX2 code path is in use on this CPU.
bool simd_level_avx2() noexcept;

// Whole-string wildcard match: '*' any run, '?' one character.
bool glob(std::string_view s, std::string_view pattern) noexcept;
// Longest wildcard-free piece of a pattern (used as a fast prefilter).
std::string_view longest_literal(std::string_view pattern) noexcept;

// Fuzzy subsequence match: every needle byte appears in order. Returns a
// quality score in [0, 199], or -1 if it doesn't match.
int fuzzy(std::string_view hay, std::string_view needle) noexcept;

// How well `term` matches a name:
//   1000 exact, 920 exact without extension, 800 prefix, 650 start of a word,
//   500 anywhere, 0 no match.
// `original` (unfolded) enables camelCase word-boundary detection.
struct Tier {
    int score = 0;
    size_t pos = npos;
};
Tier name_tier(std::string_view folded, std::string_view original, std::string_view term) noexcept;

// Does `term` spell the first letters of consecutive words?
// ("vsc" -> "Visual Studio Code", "ps" -> "PhotoShop")
bool initials(std::string_view folded, std::string_view original, std::string_view term) noexcept;

// Which code points of `original` to highlight for this query.
// Returns one flag per code point of `original`.
std::vector<uint8_t> highlight(const Query& q, std::string_view original, bool fuzzy);

}  // namespace match
}  // namespace engine
