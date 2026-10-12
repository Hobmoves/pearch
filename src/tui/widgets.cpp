#include "tui/widgets.h"

#include <algorithm>
#include <cmath>

#include "core/text.h"
#include "tui/theme.h"

namespace tui {

namespace {
const char* const kLogo[kWordmarkHeight] = {
    "██████╗ ███████╗ █████╗ ██████╗  ██████╗██╗  ██╗",
    "██╔══██╗██╔════╝██╔══██╗██╔══██╗██╔════╝██║  ██║",
    "██████╔╝█████╗  ███████║██████╔╝██║     ███████║",
    "██╔═══╝ ██╔══╝  ██╔══██║██╔══██╗██║     ██╔══██║",
    "██║     ███████╗██║  ██║██║  ██║╚██████╗██║  ██║",
    "╚═╝     ╚══════╝╚═╝  ╚═╝╚═╝  ╚═╝ ╚═════╝╚═╝  ╚═╝",
};
}  // namespace

void draw_wordmark(Canvas& c, int x, int y, bool color) {
    for (int row = 0; row < kWordmarkHeight; ++row) {
        std::string_view line = kLogo[row];
        int col = 0;
        for (size_t i = 0; i < line.size(); ++col) {
            char32_t cp = text::decode_utf8(line, i);
            if (cp == U' ') continue;
            float t = static_cast<float>(col) / (kWordmarkWidth - 1) * 0.85f + static_cast<float>(row) * 0.03f;
            uint32_t base = mix(theme::kLime, theme::kGold, t);
            Style st = color ? fg(cp == U'█' ? base : scale(base, 0.42f), cp == U'█' ? kBold : 0)
                             : Style{kDefault, kDefault, static_cast<uint8_t>(cp == U'█' ? kBold : kDim)};
            c.put(x + col, y + row, cp, st);
        }
    }
}

int draw_small_wordmark(Canvas& c, int x, int y, bool color, bool spaced) {
    const char* letters = "pearch";
    int col = 0;
    for (int i = 0; i < 6; ++i) {
        uint32_t cc = mix(theme::kLime, theme::kGold, static_cast<float>(i) / 5.0f);
        c.put(x + col, y, static_cast<char32_t>(letters[i]), color ? fg(cc, kBold) : Style{kDefault, kDefault, kBold});
        col += spaced ? 2 : 1;
    }
    return spaced ? 11 : 6;
}

void draw_progress_bar(Canvas& c, int x, int y, int w, double frac, bool color) {
    if (w <= 0) return;
    frac = std::clamp(frac, 0.0, 1.0);
    const double cells = frac * w;
    const int full = static_cast<int>(cells);
    const bool half = (cells - full) >= 0.5 && full < w;
    for (int i = 0; i < w; ++i) {
        float t = w > 1 ? static_cast<float>(i) / (w - 1) : 0.0f;
        uint32_t on = mix(theme::kLime, theme::kGold, t);
        if (i < full) {
            c.put(x + i, y, U'━', color ? fg(on) : Style{kDefault, kDefault, kBold});
        } else if (i == full && half) {
            c.put(x + i, y, U'╸', color ? fg(on) : Style{kDefault, kDefault, kBold});
        } else {
            c.put(x + i, y, color ? U'━' : U'─', color ? fg(theme::kBorder) : Style{kDefault, kDefault, kDim});
        }
    }
}

void draw_sparkline(Canvas& c, int x, int y, int w, const std::vector<double>& values, uint32_t color,
                    bool use_color) {
    static const char32_t bars[] = {U'▁', U'▂', U'▃', U'▄', U'▅', U'▆', U'▇', U'█'};
    if (values.empty() || w <= 0) return;
    double mx = *std::max_element(values.begin(), values.end());
    if (mx <= 0) mx = 1;
    int n = std::min<int>(w, static_cast<int>(values.size()));
    for (int i = 0; i < n; ++i) {
        double v = values[values.size() - n + i] / mx;
        int idx = std::clamp(static_cast<int>(v * 7.0 + 0.5), 0, 7);
        float fade = 0.45f + 0.55f * static_cast<float>(i + 1) / n;
        c.put(x + i, y, bars[idx], use_color ? fg(scale(color, fade)) : Style{});
    }
}

char32_t spinner(double t) {
    static const char32_t frames[] = {U'⠋', U'⠙', U'⠹', U'⠸', U'⠼', U'⠴', U'⠦', U'⠧', U'⠇', U'⠏'};
    return frames[static_cast<int>(t * 12.0) % 10];
}

}  // namespace tui
