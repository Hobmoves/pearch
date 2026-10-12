#include "core/text.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace text {

char32_t decode_utf8(std::string_view s, size_t& i) noexcept {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const size_t n = s.size();
    unsigned char c = p[i];
    if (c < 0x80) { ++i; return c; }
    int len = 0;
    char32_t cp = 0;
    if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
    else { ++i; return 0xFFFD; }
    if (i + len > n) { ++i; return 0xFFFD; }
    for (int k = 1; k < len; ++k) {
        unsigned char cc = p[i + k];
        if ((cc & 0xC0) != 0x80) { ++i; return 0xFFFD; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    i += len;
    return cp;
}

void append_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x110000) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        append_utf8(out, 0xFFFD);
    }
}

std::string to_utf8(std::wstring_view w) {
    std::string out;
    out.reserve(w.size());
    for (size_t i = 0; i < w.size(); ++i) {
        char32_t cp = static_cast<char32_t>(w[i]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size()) {
                char32_t lo = static_cast<char32_t>(w[i + 1]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ++i;
                }
            }
        }
        append_utf8(out, cp);
    }
    return out;
}

std::wstring to_wide(std::string_view s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        char32_t cp = decode_utf8(s, i);
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp >= 0x10000) {
                cp -= 0x10000;
                out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
                out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
                continue;
            }
        }
        out.push_back(static_cast<wchar_t>(cp));
    }
    return out;
}

namespace {

// U+00C0..U+00FF -> base letters. '*' = keep as is (× ÷), multi-letter cases
// are handled separately.
constexpr const char* kLatin1 =
    "aaaaaa?ceeeeiiii"   // C0-CF (C6 Æ handled separately)
    "dnooooo*ouuuuy??"   // D0-DF (DE Þ, DF ß separately)
    "aaaaaa?ceeeeiiii"   // E0-EF
    "dnooooo*ouuuuy?y";  // F0-FF

// U+0100..U+017F -> base letters ('?' = ij, '!' = oe).
constexpr const char* kLatinExtA =
    "aaaaaa" "cccccccc" "dddd" "eeeeeeeeee" "gggggggg" "hhhh" "iiiiiiiiii" "??"
    "jj" "kkk" "llllllllll" "nnnnnnnnn" "oooooo" "!!" "rrrrrr" "ssssssss"
    "tttttt" "uuuuuuuuuuuu" "ww" "yyy" "zzzzzz" "s";

}  // namespace

void fold_append(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        char c = static_cast<char>(cp);
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        out.push_back(c);
        return;
    }
    if (cp >= 0x0300 && cp <= 0x036F) return;  // combining diacritics
    if (cp >= 0xC0 && cp <= 0xFF) {
        switch (cp) {
            case 0xC6: case 0xE6: out += "ae"; return;
            case 0xDE: case 0xFE: out += "th"; return;
            case 0xDF: out += "ss"; return;
            default: break;
        }
        char c = kLatin1[cp - 0xC0];
        if (c == '*') append_utf8(out, cp);
        else out.push_back(c);
        return;
    }
    if (cp >= 0x100 && cp <= 0x17F) {
        char c = kLatinExtA[cp - 0x100];
        if (c == '?') out += "ij";
        else if (c == '!') out += "oe";
        else out.push_back(c);
        return;
    }
    // Greek
    if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) { append_utf8(out, cp + 0x20); return; }
    switch (cp) {
        case 0x386: case 0x3AC: append_utf8(out, 0x3B1); return;
        case 0x388: case 0x3AD: append_utf8(out, 0x3B5); return;
        case 0x389: case 0x3AE: append_utf8(out, 0x3B7); return;
        case 0x38A: case 0x3AF: append_utf8(out, 0x3B9); return;
        case 0x38C: case 0x3CC: append_utf8(out, 0x3BF); return;
        case 0x38E: case 0x3CD: append_utf8(out, 0x3C5); return;
        case 0x38F: case 0x3CE: append_utf8(out, 0x3C9); return;
        case 0x3C2: append_utf8(out, 0x3C3); return;
        // Cyrillic ё/Ё -> е
        case 0x401: case 0x451: append_utf8(out, 0x435); return;
        default: break;
    }
    // Cyrillic
    if (cp >= 0x410 && cp <= 0x42F) { append_utf8(out, cp + 0x20); return; }
    if (cp >= 0x400 && cp <= 0x40F) { append_utf8(out, cp + 0x50); return; }
    // Fullwidth forms
    if (cp >= 0xFF21 && cp <= 0xFF3A) { out.push_back(static_cast<char>('a' + (cp - 0xFF21))); return; }
    if (cp >= 0xFF41 && cp <= 0xFF5A) { out.push_back(static_cast<char>('a' + (cp - 0xFF41))); return; }
    if (cp >= 0xFF10 && cp <= 0xFF19) { out.push_back(static_cast<char>('0' + (cp - 0xFF10))); return; }
    append_utf8(out, cp);
}

std::string fold(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {  // fast path
            out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : static_cast<char>(c));
            ++i;
            continue;
        }
        fold_append(out, decode_utf8(s, i));
    }
    return out;
}

std::vector<uint32_t> fold_origin(std::string_view s, std::string& folded_out) {
    folded_out.clear();
    std::vector<uint32_t> origin;
    origin.reserve(s.size());
    uint32_t cp_index = 0;
    for (size_t i = 0; i < s.size(); ++cp_index) {
        size_t before = folded_out.size();
        fold_append(folded_out, decode_utf8(s, i));
        for (size_t k = before; k < folded_out.size(); ++k) origin.push_back(cp_index);
    }
    return origin;
}

namespace {

struct MaskTable {
    uint64_t bit[256];
    MaskTable() {
        for (int c = 0; c < 256; ++c) {
            int b;
            if (c >= 'a' && c <= 'z') b = c - 'a';
            else if (c >= '0' && c <= '9') b = 26 + (c - '0');
            else if (c == '.') b = 36;
            else if (c == '_') b = 37;
            else if (c == '-') b = 38;
            else if (c == ' ') b = 39;
            else if (c < 0x80) b = 40 + (c % 2);
            else b = 42 + (c % 22);
            bit[c] = uint64_t{1} << b;
        }
    }
};
const MaskTable kMask;

}  // namespace

uint64_t char_mask(std::string_view folded) noexcept {
    uint64_t m = 0;
    for (unsigned char c : folded) m |= kMask.bit[c];
    return m;
}

namespace {

struct Range { char32_t lo, hi; };

constexpr Range kZeroWidth[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF},
    {0x05C1, 0x05C2}, {0x05C4, 0x05C5}, {0x05C7, 0x05C7}, {0x0610, 0x061A},
    {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4},
    {0x0900, 0x0902}, {0x093A, 0x093A}, {0x093C, 0x093C}, {0x0941, 0x0948},
    {0x094D, 0x094D}, {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E},
    {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x200B, 0x200F}, {0x202A, 0x202E},
    {0x2060, 0x2064}, {0x20D0, 0x20FF}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F},
    {0xFEFF, 0xFEFF}, {0xE0100, 0xE01EF},
};

constexpr Range kWide[] = {
    {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC},
    {0x23F0, 0x23F0}, {0x23F3, 0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615},
    {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1},
    {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26CE, 0x26CE},
    {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5},
    {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B},
    {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755},
    {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF},
    {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x2E80, 0x303E},
    {0x3041, 0x33FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xA000, 0xA4CF},
    {0xA960, 0xA97F}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFE10, 0xFE19},
    {0xFE30, 0xFE6F}, {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6}, {0x16FE0, 0x16FE4},
    {0x17000, 0x18CFF}, {0x1B000, 0x1B2FF}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF},
    {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F200, 0x1F251}, {0x1F300, 0x1F320},
    {0x1F32D, 0x1F335}, {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393}, {0x1F3A0, 0x1F3CA},
    {0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0}, {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F43E},
    {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC}, {0x1F4FF, 0x1F53D}, {0x1F54B, 0x1F54E},
    {0x1F550, 0x1F567}, {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596}, {0x1F5A4, 0x1F5A4},
    {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5}, {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2},
    {0x1F6D5, 0x1F6D7}, {0x1F6DC, 0x1F6DF}, {0x1F6EB, 0x1F6EC}, {0x1F6F4, 0x1F6FC},
    {0x1F7E0, 0x1F7EB}, {0x1F7F0, 0x1F7F0}, {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945},
    {0x1F947, 0x1F9FF}, {0x1FA70, 0x1FAFF}, {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
};

template <size_t N>
bool in_table(const Range (&table)[N], char32_t cp) noexcept {
    size_t lo = 0, hi = N;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cp > table[mid].hi) lo = mid + 1;
        else if (cp < table[mid].lo) hi = mid;
        else return true;
    }
    return false;
}

}  // namespace

int char_width(char32_t cp) noexcept {
    if (cp < 0x300) return cp == 0 ? 0 : 1;
    if (in_table(kZeroWidth, cp)) return 0;
    if (in_table(kWide, cp)) return 2;
    return 1;
}

int display_width(std::string_view s) noexcept {
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++w; ++i; continue; }
        w += char_width(decode_utf8(s, i));
    }
    return w;
}

std::string truncate_end(std::string_view s, int max_width) {
    if (max_width <= 0) return {};
    if (display_width(s) <= max_width) return std::string(s);
    std::string out;
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        size_t start = i;
        char32_t cp = decode_utf8(s, i);
        int cw = char_width(cp);
        if (w + cw > max_width - 1) break;
        out.append(s.substr(start, i - start));
        w += cw;
    }
    out += "\xE2\x80\xA6";  // …
    return out;
}

std::string truncate_middle(std::string_view s, int max_width) {
    if (max_width <= 0) return {};
    if (display_width(s) <= max_width) return std::string(s);
    if (max_width == 1) return "\xE2\x80\xA6";
    int head_w = (max_width - 1) / 2;
    int tail_w = max_width - 1 - head_w;

    // Collect code points with their widths.
    struct Cp { size_t start, end; int w; };
    std::vector<Cp> cps;
    for (size_t i = 0; i < s.size();) {
        size_t st = i;
        char32_t cp = decode_utf8(s, i);
        cps.push_back({st, i, char_width(cp)});
    }
    std::string head, tail;
    int w = 0;
    size_t hi = 0;
    for (; hi < cps.size() && w + cps[hi].w <= head_w; ++hi) w += cps[hi].w;
    head.assign(s.substr(0, hi ? cps[hi - 1].end : 0));
    w = 0;
    size_t ti = cps.size();
    while (ti > hi && w + cps[ti - 1].w <= tail_w) { --ti; w += cps[ti].w; }
    if (ti < cps.size()) tail.assign(s.substr(cps[ti].start));
    return head + "\xE2\x80\xA6" + tail;
}

std::string truncate_path(std::string_view s, int max_width, char sep) {
    if (max_width <= 0) return {};
    if (display_width(s) <= max_width) return std::string(s);
    auto parts = split(s, sep);
    if (parts.size() < 3) return truncate_middle(s, max_width);
    const std::string ell = "\xE2\x80\xA6";
    std::string head = parts.front();
    // Try to keep as many trailing components as possible.
    std::string best;
    for (size_t k = 1; k < parts.size() - 1; ++k) {
        std::string tail;
        for (size_t j = parts.size() - k; j < parts.size(); ++j) {
            tail += sep;
            tail += parts[j];
        }
        std::string candidate = head + sep + ell + tail;
        if (display_width(candidate) <= max_width) best = std::move(candidate);
        else break;
    }
    if (!best.empty()) return best;
    std::string last = ell + std::string(1, sep) + parts.back();
    if (display_width(last) <= max_width) return last;
    return truncate_middle(s, max_width);
}

std::string pad_right(std::string_view s, int width) {
    std::string out(s);
    int w = display_width(s);
    if (w < width) out.append(static_cast<size_t>(width - w), ' ');
    return out;
}

std::string pad_left(std::string_view s, int width) {
    int w = display_width(s);
    std::string out;
    if (w < width) out.append(static_cast<size_t>(width - w), ' ');
    out.append(s);
    return out;
}

std::string_view trim(std::string_view s) noexcept {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            out.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

static char lower_ascii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

bool starts_with_ci(std::string_view s, std::string_view prefix) noexcept {
    if (prefix.size() > s.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (lower_ascii(s[i]) != lower_ascii(prefix[i])) return false;
    return true;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() && starts_with_ci(a, b);
}

std::string to_lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = lower_ascii(c);
    return out;
}

std::string_view extension_of(std::string_view name) noexcept {
    size_t dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == name.size()) return {};
    return name.substr(dot + 1);
}

}  // namespace text
