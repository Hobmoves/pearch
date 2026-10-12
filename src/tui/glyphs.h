#pragma once

// Box drawing, spinners and symbols. Modern terminals (Windows Terminal,
// VS Code, macOS/Linux terminals) get the rich set; the classic Windows
// console with Consolas lacks rounded corners, braille, ✓ and friends, so it
// gets look-alikes from the WGL4 set it can draw. PEARCH_GLYPHS=rich|basic
// overrides the detection.

#include <cstdlib>
#include <string>

namespace tui {

struct Glyphs {
    bool rich;
    char32_t tl, tr, bl, br;        // box corners
    const char* check;               // done / ok
    const char* cross;               // failed
    const char* prompt;              // search box prompt
    const char* enter;               // the Enter key
    char32_t bar_on, bar_half, bar_off;  // progress bar
    char32_t thumb;                  // scrollbar thumb
    char32_t sparkle;
    const char32_t* spinner;         // frames
    int spinner_frames;
    const char32_t* spark_bars;      // sparkline, 8 levels
};

inline const Glyphs& glyphs() {
    static const Glyphs g = [] {
        bool rich = true;
#ifdef _WIN32
        rich = std::getenv("WT_SESSION") || std::getenv("TERM_PROGRAM") || std::getenv("WEZTERM_EXECUTABLE") ||
               std::getenv("ALACRITTY_LOG");
#endif
        if (const char* e = std::getenv("PEARCH_GLYPHS")) rich = std::string(e) != "basic";
        static const char32_t braille[] = {U'⠋', U'⠙', U'⠹', U'⠸', U'⠼', U'⠴', U'⠦', U'⠧', U'⠇', U'⠏'};
        static const char32_t ascii_spin[] = {U'|', U'/', U'-', U'\\'};
        static const char32_t bars_rich[] = {U'▁', U'▂', U'▃', U'▄', U'▅', U'▆', U'▇', U'█'};
        static const char32_t bars_basic[] = {U'_', U'_', U'▄', U'▄', U'▄', U'█', U'█', U'█'};
        if (rich)
            return Glyphs{true, U'╭', U'╮', U'╰', U'╯', "✓", "✗", "❯", "⏎", U'━', U'╸', U'━', U'┃', U'✦',
                          braille, 10, bars_rich};
        return Glyphs{false, U'┌', U'┐', U'└', U'┘', "√", "x", ">", "Enter", U'█', U'▌', U'░', U'█', U'*',
                      ascii_spin, 4, bars_basic};
    }();
    return g;
}

}  // namespace tui
