#include "engine/matcher.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <string>

#include "core/text.h"
#include "engine/query.h"

#include "core/hardware.h"

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define PEARCH_HAS_SSE2 1
#endif

#if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__)
#include <immintrin.h>
#define PEARCH_HAS_AVX2 1
#define PEARCH_AVX2_TARGET __attribute__((target("avx2")))
#elif defined(_MSC_VER) && defined(_M_X64)
#include <immintrin.h>
#define PEARCH_HAS_AVX2 1
#define PEARCH_AVX2_TARGET
#endif

namespace engine::match {

#ifdef PEARCH_HAS_AVX2
// 32 positions per step. Only called when the CPU reports AVX2 (checked once
// at runtime), so one binary runs everywhere and uses what it finds.
PEARCH_AVX2_TARGET static size_t find_avx2(const char* s, size_t n, const char* needle, size_t k) noexcept {
    size_t i = 0;
    if (n >= k + 31) {
        const __m256i first = _mm256_set1_epi8(needle[0]);
        const __m256i last = _mm256_set1_epi8(needle[k - 1]);
        const size_t limit = n - k - 31;
        for (; i <= limit; i += 32) {
            const __m256i bf = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + i));
            const __m256i bl = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + i + k - 1));
            auto mask = static_cast<unsigned>(
                _mm256_movemask_epi8(_mm256_and_si256(_mm256_cmpeq_epi8(first, bf), _mm256_cmpeq_epi8(last, bl))));
            while (mask) {
                const unsigned bit = static_cast<unsigned>(std::countr_zero(mask));
                if (std::memcmp(s + i + bit + 1, needle + 1, k - 2) == 0) return i + bit;
                mask &= mask - 1;
            }
        }
    }
    for (; i + k <= n; ++i)
        if (s[i] == needle[0] && s[i + k - 1] == needle[k - 1] && std::memcmp(s + i + 1, needle + 1, k - 2) == 0)
            return i;
    return npos;
}
#endif

bool simd_level_avx2() noexcept {
#ifdef PEARCH_HAS_AVX2
    static const bool on = hw::info().avx2;
    return on;
#else
    return false;
#endif
}

size_t find(const char* s, size_t n, const char* needle, size_t k) noexcept {
    if (k == 0) return 0;
    if (k > n) return npos;
    if (k == 1) {
        if (n < 48) {  // short names: a plain loop beats the memchr call
            for (size_t i = 0; i < n; ++i)
                if (s[i] == needle[0]) return i;
            return npos;
        }
        const void* p = std::memchr(s, needle[0], n);
        return p ? static_cast<size_t>(static_cast<const char*>(p) - s) : npos;
    }
#ifdef PEARCH_HAS_AVX2
    if (n >= 64 && simd_level_avx2()) return find_avx2(s, n, needle, k);
#endif
    size_t i = 0;
#ifdef PEARCH_HAS_SSE2
    if (n >= k + 15) {
        const __m128i first = _mm_set1_epi8(needle[0]);
        const __m128i last = _mm_set1_epi8(needle[k - 1]);
        const size_t limit = n - k - 15;  // last i for which both loads stay in bounds
        for (; i <= limit; i += 16) {
            const __m128i bf = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i));
            const __m128i bl = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i + k - 1));
            auto mask = static_cast<unsigned>(
                _mm_movemask_epi8(_mm_and_si128(_mm_cmpeq_epi8(first, bf), _mm_cmpeq_epi8(last, bl))));
            while (mask) {
                const unsigned bit = static_cast<unsigned>(std::countr_zero(mask));
                if (std::memcmp(s + i + bit + 1, needle + 1, k - 2) == 0) return i + bit;
                mask &= mask - 1;
            }
        }
    }
#endif
    const char c0 = needle[0];
    const char cl = needle[k - 1];
    while (i + k <= n) {
        const void* p = std::memchr(s + i, c0, n - k + 1 - i);
        if (!p) return npos;
        i = static_cast<size_t>(static_cast<const char*>(p) - s);
        if (s[i + k - 1] == cl && std::memcmp(s + i + 1, needle + 1, k - 2) == 0) return i;
        ++i;
    }
    return npos;
}

static size_t cp_len(unsigned char c) noexcept {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

bool glob(std::string_view s, std::string_view p) noexcept {
    size_t si = 0, pi = 0, star = npos, mark = 0;
    while (si < s.size()) {
        if (pi < p.size() && p[pi] == '?') {
            si += cp_len(static_cast<unsigned char>(s[si]));
            ++pi;
        } else if (pi < p.size() && p[pi] == '*') {
            star = pi++;
            mark = si;
        } else if (pi < p.size() && p[pi] == s[si]) {
            ++si;
            ++pi;
        } else if (star != npos) {
            pi = star + 1;
            mark += cp_len(static_cast<unsigned char>(s[mark]));
            si = mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

std::string_view longest_literal(std::string_view p) noexcept {
    std::string_view best;
    size_t i = 0;
    while (i < p.size()) {
        while (i < p.size() && (p[i] == '*' || p[i] == '?')) ++i;
        size_t j = i;
        while (j < p.size() && p[j] != '*' && p[j] != '?') ++j;
        if (j - i > best.size()) best = p.substr(i, j - i);
        i = j;
    }
    return best;
}

static bool is_separator(char c) noexcept {
    switch (c) {
        case ' ': case '_': case '-': case '.': case '(': case ')': case '[': case ']':
        case '{': case '}': case '+': case ',': case ';': case '\'': case '&': case '@':
        case '#': case '!': case '~':
            return true;
        default:
            return false;
    }
}

static bool is_boundary(std::string_view folded, std::string_view original, size_t pos) noexcept {
    if (pos == 0) return true;
    if (is_separator(folded[pos - 1])) return true;
    // camelCase / digit transitions, only when folding kept byte positions.
    if (original.size() == folded.size() && pos < original.size()) {
        char prev = original[pos - 1], cur = original[pos];
        if (cur >= 'A' && cur <= 'Z' && prev >= 'a' && prev <= 'z') return true;
        bool prev_digit = prev >= '0' && prev <= '9';
        bool cur_digit = cur >= '0' && cur <= '9';
        if (prev_digit != cur_digit && ((cur | 32) >= 'a' && (cur | 32) <= 'z')) return true;
        if (cur_digit && !prev_digit) return true;
    }
    return false;
}

Tier name_tier(std::string_view name, std::string_view original, std::string_view term) noexcept {
    if (term.empty()) return {500, 0};
    if (name == term) return {1000, 0};
    size_t first = find(name, term);
    if (first == npos) return {};
    if (first == 0) {
        size_t dot = name.rfind('.');
        if (dot != std::string_view::npos && dot == term.size()) return {920, 0};
        return {800, 0};
    }
    if (is_boundary(name, original, first)) return {650, first};
    // Look for a later occurrence that starts a word ("q3 report" in "my_report").
    const char c0 = term[0];
    const size_t last = name.size() - term.size();
    for (size_t pos = first + 1; pos <= last; ++pos) {
        if (name[pos] != c0 || !is_boundary(name, original, pos)) continue;
        if (name.compare(pos, term.size(), term) == 0) return {650, pos};
    }
    return {500, first};
}

// Greedy subsequence positions, tightened with a backward pass (like fzf v1).
static bool fuzzy_positions(std::string_view hay, std::string_view needle, size_t* pos) noexcept {
    const size_t m = needle.size();
    size_t j = 0, end = npos;
    for (size_t i = 0; i < hay.size() && j < m; ++i) {
        if (hay[i] == needle[j]) {
            if (++j == m) end = i;
        }
    }
    if (j < m) return false;
    // Walk backwards from the end to find the latest start.
    size_t start = 0;
    j = m;
    for (size_t i = end + 1; i-- > 0;) {
        if (hay[i] == needle[j - 1]) {
            if (--j == 0) { start = i; break; }
        }
    }
    j = 0;
    for (size_t i = start; i <= end && j < m; ++i)
        if (hay[i] == needle[j]) pos[j++] = i;
    return j == m;
}

int fuzzy(std::string_view hay, std::string_view needle) noexcept {
    if (needle.empty()) return 0;
    if (needle.size() > hay.size() || needle.size() > 64) return -1;
    size_t pos[64];
    if (!fuzzy_positions(hay, needle, pos)) return -1;
    // Letters scattered across a long name are a coincidence, not a match:
    // an abbreviation starts a word ("qrtly" -> "quarterly") and comes in
    // few contiguous runs.
    const size_t m = needle.size();
    if (pos[m - 1] - pos[0] + 1 > m * 2 + 2) return -1;
    size_t runs = 1;
    for (size_t k = 1; k < m; ++k) runs += pos[k] != pos[k - 1] + 1;
    const bool starts_word = pos[0] == 0 || is_separator(hay[pos[0] - 1]);
    if (runs > (starts_word ? m / 2 + 1 : 2)) return -1;
    int score = 100;
    for (size_t k = 0; k < needle.size(); ++k) {
        size_t p = pos[k];
        if (p == 0 || is_separator(hay[p - 1])) score += 8;
        if (k > 0) {
            size_t gap = p - pos[k - 1] - 1;
            if (gap == 0) score += 6;
            else score -= static_cast<int>(std::min<size_t>(gap, 4));
        }
    }
    score -= static_cast<int>(std::min<size_t>(pos[0], 10));
    score -= static_cast<int>(std::min<size_t>(hay.size() - needle.size(), 20)) / 2;
    return std::clamp(score, 0, 199);
}

bool initials(std::string_view folded, std::string_view original, std::string_view term) noexcept {
    if (term.size() < 2 || term.size() > 8) return false;
    char buf[64];
    size_t n = 0;
    for (size_t i = 0; i < folded.size() && n < 64; ++i) {
        if (is_separator(folded[i])) continue;
        if (is_boundary(folded, original, i)) buf[n++] = folded[i];
    }
    return std::string_view(buf, n).find(term) != std::string_view::npos;
}

static void mark_initials(std::string_view folded, std::string_view original, std::string_view term,
                          const std::vector<uint32_t>& origin, std::vector<uint8_t>& marks) {
    std::string init;
    std::vector<size_t> where;
    for (size_t i = 0; i < folded.size(); ++i) {
        if (is_separator(folded[i]) || !is_boundary(folded, original, i)) continue;
        init.push_back(folded[i]);
        where.push_back(i);
    }
    size_t at = init.find(term);
    if (at == std::string::npos) return;
    for (size_t k = 0; k < term.size(); ++k)
        if (where[at + k] < origin.size()) marks[origin[where[at + k]]] = 1;
}

std::vector<uint8_t> highlight(const Query& q, std::string_view original, bool use_fuzzy) {
    std::string folded;
    std::vector<uint32_t> origin = text::fold_origin(original, folded);
    size_t cps = 0;
    for (size_t i = 0; i < original.size(); ++cps) text::decode_utf8(original, i);
    std::vector<uint8_t> marks(cps, 0);
    auto mark_range = [&](size_t start, size_t len) {
        for (size_t b = start; b < start + len && b < origin.size(); ++b) marks[origin[b]] = 1;
    };
    for (const Term& t : q.terms) {
        if (t.negate || t.text.empty()) continue;
        if (t.kind == Term::Substr) {
            Tier tier = name_tier(folded, original, t.text);
            if (tier.score > 0) {
                mark_range(tier.pos, t.text.size());
            } else if (initials(folded, original, t.text)) {
                mark_initials(folded, original, t.text, origin, marks);
            } else if (use_fuzzy && t.text.size() <= 64) {
                size_t pos[64];
                if (fuzzy_positions(folded, t.text, pos))
                    for (size_t k = 0; k < t.text.size(); ++k) mark_range(pos[k], 1);
            }
        } else if (t.kind == Term::Wildcard) {
            // Highlight the literal pieces left to right.
            size_t from = 0, i = 0;
            std::string_view p = t.text;
            while (i < p.size()) {
                while (i < p.size() && (p[i] == '*' || p[i] == '?')) ++i;
                size_t j = i;
                while (j < p.size() && p[j] != '*' && p[j] != '?') ++j;
                if (j > i) {
                    size_t at = find(std::string_view(folded).substr(from), p.substr(i, j - i));
                    if (at == npos) break;
                    mark_range(from + at, j - i);
                    from += at + (j - i);
                }
                i = j;
            }
        }
    }
    return marks;
}

}  // namespace engine::match
