#pragma once

// A cell canvas plus a diffing renderer. Views draw a whole frame into a
// Canvas; the Renderer emits only the cells that changed since the previous
// frame, wrapped in a synchronized-update block, so nothing flickers.

#include <climits>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "platform/console.h"

namespace tui {

constexpr uint32_t kDefault = 0xFF000000u;  // terminal default colour

enum Attr : uint8_t { kBold = 1, kDim = 2, kItalic = 4, kUnderline = 8, kReverse = 16 };

struct Style {
    uint32_t fg = kDefault;
    uint32_t bg = kDefault;
    uint8_t attr = 0;
    bool operator==(const Style&) const = default;
};

inline Style fg(uint32_t c, uint8_t attr = 0) { return {c, kDefault, attr}; }

struct Cell {
    char32_t ch = U' ';
    Style style;
    uint8_t width = 1;  // 2 = wide character, 0 = right half of a wide character
    bool operator==(const Cell&) const = default;
};

class Canvas {
public:
    Canvas(int w = 0, int h = 0) { resize(w, h); }
    void resize(int w, int h);
    int width() const noexcept { return w_; }
    int height() const noexcept { return h_; }
    void clear(Style st = {});
    const Cell& at(int x, int y) const { return cells_[static_cast<size_t>(y) * w_ + x]; }
    Cell& at(int x, int y) { return cells_[static_cast<size_t>(y) * w_ + x]; }

    void put(int x, int y, char32_t ch, Style st);
    // Draws UTF-8 text clipped to max_w columns; returns the columns used.
    int text(int x, int y, std::string_view s, Style st, int max_w = INT_MAX);
    void fill(int x, int y, int w, int h, char32_t ch, Style st);
    void fill_bg(int x, int y, int w, int h, uint32_t bg);
    void box(int x, int y, int w, int h, Style st);

private:
    int w_ = 0, h_ = 0;
    std::vector<Cell> cells_;
};

class Renderer {
public:
    explicit Renderer(term::ColorMode mode) : mode_(mode) {}
    term::ColorMode mode() const noexcept { return mode_; }
    // Escape sequence that turns `prev` into `next`.
    std::string diff(const Canvas& prev, const Canvas& next, bool full_redraw) const;
    std::string sgr(const Style& s) const;

private:
    term::ColorMode mode_;
};

// Colour helpers (0xRRGGBB).
uint32_t mix(uint32_t a, uint32_t b, float t);
uint32_t scale(uint32_t c, float k);
uint8_t to_xterm256(uint32_t c);

// A canvas as plain lines of text with colour escapes (for normal,
// scrolling output such as `pearch tour`).
std::vector<std::string> to_lines(const Canvas& c, term::ColorMode mode);

// Coloured text for plain (line-by-line) output; resets afterwards.
// ColorMode::None returns the text unchanged.
std::string paint(std::string_view text, Style st, term::ColorMode mode);

}  // namespace tui
