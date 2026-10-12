#pragma once

// Small reusable pieces: the wordmark, progress bar, sparkline, spinner.

#include <string>
#include <vector>

#include "tui/screen.h"

namespace tui {

// Big 6-row block-letter "PEARCH" (48 columns) with a lime->gold gradient.
constexpr int kWordmarkWidth = 48;
constexpr int kWordmarkHeight = 6;
void draw_wordmark(Canvas& c, int x, int y, bool color);
// One-line "p e a r c h" title (11 columns).
int draw_small_wordmark(Canvas& c, int x, int y, bool color, bool spaced = true);

// Thin "━━━━╸────" style bar with half-cell precision.
void draw_progress_bar(Canvas& c, int x, int y, int w, double frac, bool color);

// "▁▂▃▅▇" style mini chart of the given values.
void draw_sparkline(Canvas& c, int x, int y, int w, const std::vector<double>& values, uint32_t color, bool use_color);

// Braille spinner frame for time t (seconds).
char32_t spinner(double t);

}  // namespace tui
