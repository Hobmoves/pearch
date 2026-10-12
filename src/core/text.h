#pragma once

// Text utilities shared by the whole app: UTF-8 <-> wide conversion, the
// case/diacritic folding used for matching, terminal display widths and
// width-aware truncation. Everything here is portable and allocation-light.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace text {

// Decode one code point starting at s[i]; advances i. Invalid bytes decode as
// U+FFFD and consume one byte, so loops always make progress.
char32_t decode_utf8(std::string_view s, size_t& i) noexcept;
void append_utf8(std::string& out, char32_t cp);

std::string to_utf8(std::wstring_view w);
std::wstring to_wide(std::string_view s);

// Lowercase + strip accents ("Café_Ünïcode" -> "cafe_unicode"). This is the
// representation the index and every query are matched in.
std::string fold(std::string_view s);
// Fold a single code point into out (may append 0..3 bytes).
void fold_append(std::string& out, char32_t cp);

// Maps every byte of fold(s) back to the index of the code point in s that
// produced it. Used to highlight matches in the original (unfolded) name.
std::vector<uint32_t> fold_origin(std::string_view s, std::string& folded_out);

// Bitmask of the characters present in a folded string: bits 0-25 a-z,
// 26-35 0-9, plus a few punctuation/non-ASCII buckets. A query can only match
// a name whose mask is a superset of the query's mask.
uint64_t char_mask(std::string_view folded) noexcept;

// Terminal column width of a code point (0 for combining marks, 2 for wide
// East Asian characters and most emoji).
int char_width(char32_t cp) noexcept;
int display_width(std::string_view s) noexcept;

// Width-aware truncation with an ellipsis.
std::string truncate_end(std::string_view s, int max_width);
std::string truncate_middle(std::string_view s, int max_width);
// Keeps the end of a path (the most specific part): "C:\Users\me\…\Work\Q3".
std::string truncate_path(std::string_view s, int max_width, char sep);

std::string pad_right(std::string_view s, int width);
std::string pad_left(std::string_view s, int width);

std::string_view trim(std::string_view s) noexcept;
std::vector<std::string> split(std::string_view s, char sep);
bool starts_with_ci(std::string_view s, std::string_view prefix) noexcept;
bool iequals(std::string_view a, std::string_view b) noexcept;
std::string to_lower_ascii(std::string_view s);

// Extension of a filename without the dot, as written ("Report.PDF" -> "PDF").
std::string_view extension_of(std::string_view name) noexcept;

}  // namespace text
