#include "tui/screen.h"

#include <algorithm>
#include <cmath>

#include "core/text.h"

namespace tui {

void Canvas::resize(int w, int h) {
    w_ = std::max(0, w);
    h_ = std::max(0, h);
    cells_.assign(static_cast<size_t>(w_) * h_, Cell{});
}

void Canvas::clear(Style st) {
    for (auto& c : cells_) c = Cell{U' ', st, 1};
}

void Canvas::put(int x, int y, char32_t ch, Style st) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
    int cw = text::char_width(ch);
    if (cw == 2 && x + 1 >= w_) cw = 1, ch = U' ';
    Cell& c = at(x, y);
    // Don't leave half of a wide character behind.
    if (c.width == 0 && x > 0) at(x - 1, y) = Cell{U' ', at(x - 1, y).style, 1};
    if (c.width == 2 && x + 1 < w_) at(x + 1, y) = Cell{U' ', c.style, 1};
    c = Cell{ch, st, static_cast<uint8_t>(cw == 2 ? 2 : 1)};
    if (cw == 2) {
        Cell& r = at(x + 1, y);
        if (r.width == 2 && x + 2 < w_) at(x + 2, y) = Cell{U' ', r.style, 1};
        r = Cell{0, st, 0};
    }
}

int Canvas::text(int x, int y, std::string_view s, Style st, int max_w) {
    if (y < 0 || y >= h_) return 0;
    int used = 0;
    for (size_t i = 0; i < s.size();) {
        char32_t cp = text::decode_utf8(s, i);
        if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) cp = U'�';
        int cw = text::char_width(cp);
        if (cw == 0) continue;  // combining marks are not rendered separately
        if (used + cw > max_w) break;
        if (x + used >= 0 && x + used + cw <= w_) put(x + used, y, cp, st);
        used += cw;
    }
    return used;
}

void Canvas::fill(int x, int y, int w, int h, char32_t ch, Style st) {
    for (int yy = std::max(0, y); yy < std::min(h_, y + h); ++yy)
        for (int xx = std::max(0, x); xx < std::min(w_, x + w); ++xx) put(xx, yy, ch, st);
}

void Canvas::fill_bg(int x, int y, int w, int h, uint32_t bg) {
    for (int yy = std::max(0, y); yy < std::min(h_, y + h); ++yy)
        for (int xx = std::max(0, x); xx < std::min(w_, x + w); ++xx) at(xx, yy).style.bg = bg;
}

void Canvas::box(int x, int y, int w, int h, Style st) {
    if (w < 2 || h < 2) return;
    put(x, y, U'╭', st);
    put(x + w - 1, y, U'╮', st);
    put(x, y + h - 1, U'╰', st);
    put(x + w - 1, y + h - 1, U'╯', st);
    for (int i = x + 1; i < x + w - 1; ++i) {
        put(i, y, U'─', st);
        put(i, y + h - 1, U'─', st);
    }
    for (int j = y + 1; j < y + h - 1; ++j) {
        put(x, j, U'│', st);
        put(x + w - 1, j, U'│', st);
    }
}

uint32_t mix(uint32_t a, uint32_t b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto ch = [&](int shift) {
        float va = static_cast<float>((a >> shift) & 0xFF), vb = static_cast<float>((b >> shift) & 0xFF);
        return static_cast<uint32_t>(std::lround(va + (vb - va) * t)) << shift;
    };
    return ch(16) | ch(8) | ch(0);
}

uint32_t scale(uint32_t c, float k) {
    auto ch = [&](int shift) {
        float v = static_cast<float>((c >> shift) & 0xFF) * k;
        return static_cast<uint32_t>(std::clamp(std::lround(v), 0L, 255L)) << shift;
    };
    return ch(16) | ch(8) | ch(0);
}

uint8_t to_xterm256(uint32_t c) {
    const int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    static const int levels[6] = {0, 95, 135, 175, 215, 255};
    auto nearest = [](int v) {
        int best = 0;
        for (int i = 1; i < 6; ++i)
            if (std::abs(levels[i] - v) < std::abs(levels[best] - v)) best = i;
        return best;
    };
    int ri = nearest(r), gi = nearest(g), bi = nearest(b);
    int cube_d = (levels[ri] - r) * (levels[ri] - r) + (levels[gi] - g) * (levels[gi] - g) +
                 (levels[bi] - b) * (levels[bi] - b);
    int avg = (r + g + b) / 3;
    int gray_i = std::clamp((avg - 8 + 5) / 10, 0, 23);
    int gv = 8 + gray_i * 10;
    int gray_d = (gv - r) * (gv - r) + (gv - g) * (gv - g) + (gv - b) * (gv - b);
    if (gray_d < cube_d) return static_cast<uint8_t>(232 + gray_i);
    return static_cast<uint8_t>(16 + 36 * ri + 6 * gi + bi);
}

static void append_int(std::string& out, int v) {
    char buf[12];
    int n = 0;
    if (v == 0) buf[n++] = '0';
    while (v > 0) { buf[n++] = static_cast<char>('0' + v % 10); v /= 10; }
    while (n) out.push_back(buf[--n]);
}

std::string Renderer::sgr(const Style& s) const {
    std::string out = "\x1b[0";
    if (s.attr & kBold) out += ";1";
    if (s.attr & kDim) out += ";2";
    if (s.attr & kItalic) out += ";3";
    if (s.attr & kUnderline) out += ";4";
    if (s.attr & kReverse) out += ";7";
    auto color = [&](uint32_t c, bool bg) {
        if (c == kDefault || mode_ == term::ColorMode::None) return;
        if (mode_ == term::ColorMode::TrueColor) {
            out += bg ? ";48;2;" : ";38;2;";
            append_int(out, (c >> 16) & 0xFF);
            out += ';';
            append_int(out, (c >> 8) & 0xFF);
            out += ';';
            append_int(out, c & 0xFF);
        } else {
            out += bg ? ";48;5;" : ";38;5;";
            append_int(out, to_xterm256(c));
        }
    };
    color(s.fg, false);
    color(s.bg, true);
    out += 'm';
    return out;
}

std::string Renderer::diff(const Canvas& prev, const Canvas& next, bool full) const {
    std::string out;
    out.reserve(static_cast<size_t>(next.width()) * next.height() * 4);
    out += "\x1b[?2026h";  // begin synchronized update (ignored where unsupported)
    if (full || prev.width() != next.width() || prev.height() != next.height()) {
        full = true;
        out += "\x1b[0m\x1b[2J";
    }
    Style pen;
    bool pen_known = false;
    int cx = -1, cy = -1;
    for (int y = 0; y < next.height(); ++y) {
        for (int x = 0; x < next.width(); ++x) {
            const Cell& c = next.at(x, y);
            if (c.width == 0) continue;
            if (!full && c == prev.at(x, y) &&
                !(c.width == 2 && x + 1 < next.width() && !(next.at(x + 1, y) == prev.at(x + 1, y))))
                continue;
            if (cx != x || cy != y) {
                out += "\x1b[";
                append_int(out, y + 1);
                out += ';';
                append_int(out, x + 1);
                out += 'H';
            }
            if (!pen_known || !(pen == c.style)) {
                out += sgr(c.style);
                pen = c.style;
                pen_known = true;
            }
            text::append_utf8(out, c.ch ? c.ch : U' ');
            cx = x + c.width;
            cy = y;
        }
    }
    out += "\x1b[0m\x1b[?2026l";
    return out;
}

std::vector<std::string> to_lines(const Canvas& c, term::ColorMode mode) {
    Renderer r(mode);
    std::vector<std::string> out;
    for (int y = 0; y < c.height(); ++y) {
        std::string line;
        Style pen;
        bool known = false;
        int last = c.width() - 1;
        while (last >= 0 && c.at(last, y).ch == U' ' && c.at(last, y).style.bg == kDefault) --last;
        for (int x = 0; x <= last; ++x) {
            const Cell& cell = c.at(x, y);
            if (cell.width == 0) continue;
            if (mode != term::ColorMode::None && (!known || !(pen == cell.style))) {
                line += r.sgr(cell.style);
                pen = cell.style;
                known = true;
            }
            text::append_utf8(line, cell.ch ? cell.ch : U' ');
        }
        if (known) line += "\x1b[0m";
        out.push_back(std::move(line));
    }
    return out;
}

std::string paint(std::string_view s, Style st, term::ColorMode mode) {
    if (mode == term::ColorMode::None) return std::string(s);
    Renderer r(mode);
    std::string out = r.sgr(st);
    out.append(s);
    out += "\x1b[0m";
    return out;
}

}  // namespace tui
