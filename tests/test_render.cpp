#include "check.h"
#include "core/text.h"
#include "tui/index_view.h"
#include "tui/pear3d.h"
#include "tui/screen.h"

using namespace tui;

TEST(canvas_text_and_wide_chars) {
    Canvas c(10, 2);
    int used = c.text(0, 0, "日本語abc", Style{}, 7);
    CHECK_EQ(used, 7);
    CHECK_EQ(c.at(0, 0).width, uint8_t{2});
    CHECK_EQ(c.at(1, 0).width, uint8_t{0});
    c.put(1, 0, U'x', Style{});  // overwriting half a wide char clears the other half
    CHECK(c.at(0, 0).ch == U' ');
    CHECK_EQ(c.text(0, 1, "a\x01" "b", Style{}), 3);  // control characters are made visible
}

TEST(renderer_only_redraws_changes) {
    Renderer r(term::ColorMode::TrueColor);
    Canvas a(40, 10), b(40, 10);
    a.text(0, 0, "hello", fg(0xFF0000));
    b.text(0, 0, "hello", fg(0xFF0000));
    std::string full = r.diff(Canvas(), a, true);
    std::string none = r.diff(a, b, false);
    CHECK(full.find("hello") != std::string::npos);
    CHECK(none.find("hello") == std::string::npos);
    b.text(0, 0, "help!", fg(0xFF0000));
    std::string some = r.diff(a, b, false);
    CHECK(some.find("p!") != std::string::npos);
    CHECK(some.size() < full.size());
    Renderer plain(term::ColorMode::None);
    CHECK(plain.sgr(fg(0x123456)).find("38;") == std::string::npos);
    CHECK_EQ(to_xterm256(0xFFFFFF), uint8_t{231});
    CHECK_EQ(to_xterm256(0x000000), uint8_t{16});
}

static int painted(const Canvas& c) {
    int n = 0;
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x) n += c.at(x, y).ch != U' ';
    return n;
}

TEST(pear_renders_and_fills) {
    Canvas empty(44, 22), full(44, 22), mono(44, 22);
    PearParams p;
    p.time = 1.0;
    p.progress = 0.0;
    draw_pear(empty, 0, 0, 44, 22, p);
    p.progress = 1.0;
    draw_pear(full, 0, 0, 44, 22, p);
    p.color = false;
    draw_pear(mono, 0, 0, 44, 22, p);
    CHECK(painted(empty) > 200);
    CHECK(painted(mono) > 200);
    // A full pear is warmer (more red) than an empty one.
    auto red = [](const Canvas& c) {
        long sum = 0;
        for (int y = 0; y < c.height(); ++y)
            for (int x = 0; x < c.width(); ++x)
                if (c.at(x, y).style.bg != kDefault) sum += (c.at(x, y).style.bg >> 16) & 0xFF;
        return sum;
    };
    CHECK(red(full) > red(empty) * 2);
    Canvas tiny(3, 2);
    draw_pear(tiny, 0, 0, 3, 2, p);  // too small: draws nothing, no crash
}

TEST(index_frame_fits_any_size) {
    IndexFrame f;
    f.time = 2.0;
    f.progress = 0.62;
    f.entries = 412331;
    f.dirs = 38201;
    f.rate = 150000;
    f.rate_history = {1, 2, 3, 4, 5};
    f.current = "/home/me/Documents/Projects/pearch/src";
    f.title = "Indexing your files";
    f.subtitle = "~  ·  Start Menu";
    f.footer = "3.2 s elapsed";
    for (auto [w, h] : {std::pair{120, 40}, {80, 24}, {60, 20}, {40, 12}, {200, 60}, {12, 5}, {1, 1}}) {
        Canvas c(w, h);
        draw_index_frame(c, f, true);
        if (w >= 40) {
            std::string all;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) text::append_utf8(all, c.at(x, y).ch ? c.at(x, y).ch : U' ');
            CHECK(all.find("62%") != std::string::npos);
        }
    }
}
