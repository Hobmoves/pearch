#include "tui/index_view.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "core/fileio.h"
#include "core/format.h"
#include "core/text.h"
#include "tui/pear3d.h"
#include "tui/glyphs.h"
#include "tui/theme.h"
#include "tui/widgets.h"

namespace tui {

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

Style st(uint32_t color, bool use_color, uint8_t attr = 0) {
    return use_color ? Style{color, kDefault, attr} : Style{kDefault, kDefault, attr};
}

void draw_panel(Canvas& c, int x, int w, int top, int height, const IndexFrame& f, bool color) {
    const bool big = w >= kWordmarkWidth && height >= 21;
    const int content = big ? 19 : 14;
    int y = top + std::max(0, (height - content) / 2);

    if (big) {
        draw_wordmark(c, x, y, color);
        y += kWordmarkHeight + 2;
    } else {
        draw_small_wordmark(c, x, y, color);
        y += 3;
    }

    // Title with spinner / check mark.
    if (f.done) {
        c.text(x, y, glyphs().check, st(theme::kOk, color, kBold));
        c.text(x + 2, y, f.title, st(theme::kText, color, kBold), w - 2);
    } else {
        c.put(x, y, spinner(f.time), st(theme::kLime, color, kBold));
        c.text(x + 2, y, f.title, st(theme::kText, color, kBold), w - 2);
    }
    ++y;
    c.text(x + 2, y, text::truncate_end(f.subtitle, w - 2), st(theme::kMuted, color));
    y += 2;

    // Progress bar + percentage.
    const int bar_w = std::max(8, w - 7);
    draw_progress_bar(c, x, y, bar_w, f.progress, color);
    char pct[8];
    std::snprintf(pct, sizeof pct, "%3d%%", static_cast<int>(std::floor(f.progress * 100.0 + 1e-6)));
    c.text(x + bar_w + 2, y, pct, st(f.done ? theme::kOk : theme::kText, color, kBold));
    y += 2;

    // Counters.
    const int col2 = std::min(w / 2, 24);
    int used = c.text(x, y, fmt::thousands(f.entries), st(theme::kText, color, kBold));
    c.text(x + used + 1, y, "items", st(theme::kMuted, color));
    used = c.text(x + col2, y, fmt::thousands(f.dirs), st(theme::kText, color, kBold));
    c.text(x + col2 + used + 1, y, "folders", st(theme::kMuted, color));
    ++y;
    used = c.text(x, y, fmt::compact(static_cast<uint64_t>(f.rate)), st(theme::kText, color, kBold));
    c.text(x + used + 1, y, "per second", st(theme::kMuted, color));
    if (w - col2 >= 8) draw_sparkline(c, x + col2, y, std::min(20, w - col2), f.rate_history, theme::kLime, color);
    y += 2;

    // Where we are.
    if (!f.current.empty() && !f.done) {
        c.text(x, y, "›", st(theme::kLime, color));
        c.text(x + 2, y, text::truncate_path(f.current, w - 2, fileio::kPathSep), st(theme::kFaint, color));
    }
    y += 2;
    c.text(x, y, text::truncate_end(f.footer, w), st(theme::kFaint, color));
}

}  // namespace

void draw_index_frame(Canvas& c, const IndexFrame& f, bool color) {
    const int W = c.width(), H = c.height();
    c.clear();
    PearParams pp;
    pp.time = f.time;
    pp.progress = f.progress;
    pp.flash = f.flash;
    pp.color = color;
    if (W >= 74 && H >= 15) {
        const int ph = H - 2;
        const int pw = std::clamp(static_cast<int>(ph * 1.7), 24, W * 46 / 100);
        draw_pear(c, 2, 1, pw, ph, pp);
        const int px = 2 + pw + 3;
        draw_panel(c, px, W - px - 2, 0, H, f, color);
        return;
    }
    // Narrow / short terminals: pear on top, compact stats below.
    const int info = 6;
    const int ph = std::max(0, H - info - 1);
    if (ph >= 6) {
        const int pw = std::min(W - 2, static_cast<int>(ph * 1.7));
        draw_pear(c, (W - pw) / 2, 0, pw, ph, pp);
    }
    int y = std::max(0, H - info);
    const int x = 2, w = std::max(10, W - 4);
    if (f.done) c.text(x, y, glyphs().check, st(theme::kOk, color, kBold));
    else c.put(x, y, spinner(f.time), st(theme::kLime, color, kBold));
    c.text(x + 2, y, f.title, st(theme::kText, color, kBold), w - 2);
    y += 1;
    const int bar_w = std::max(6, w - 6);
    draw_progress_bar(c, x, y, bar_w, f.progress, color);
    char pct[8];
    std::snprintf(pct, sizeof pct, "%3d%%", static_cast<int>(std::floor(f.progress * 100.0 + 1e-6)));
    c.text(x + bar_w + 1, y, pct, st(theme::kText, color, kBold));
    y += 1;
    std::string counts = fmt::thousands(f.entries) + " items · " + fmt::compact(static_cast<uint64_t>(f.rate)) + "/s";
    c.text(x, y, counts, st(theme::kMuted, color), w);
    y += 1;
    if (!f.done) c.text(x, y, text::truncate_path(f.current, w, fileio::kPathSep), st(theme::kFaint, color));
    y += 1;
    c.text(x, y, text::truncate_end(f.footer, w), st(theme::kFaint, color));
}

IndexOutcome run_index_view(term::Terminal& t, term::ColorMode mode, const IndexJob& job,
                            const std::string& done_hint, double linger) {
    IndexOutcome out;
    scan::Progress prog;
    std::shared_ptr<engine::IndexStore> result;
    const auto t0 = Clock::now();
    std::thread worker([&] { result = scan::build(job.options, prog, fmt::now_ms()); });

    const bool color = mode != term::ColorMode::None;
    Renderer renderer(mode);
    Canvas prev, cur;
    bool full = true;
    double shown = 0, rate = 0, last_sample = 0, done_at = -1, last_frame = 0;
    uint64_t last_entries = 0;
    std::vector<double> history;
    std::string subtitle;
    for (const auto& l : job.labels) subtitle += (subtitle.empty() ? "" : "  ·  ") + l;
    bool quit_early = false;

    for (;;) {
        const double now = seconds_since(t0);
        const double dt = std::max(0.0, now - last_frame);
        last_frame = now;
        const bool finished = prog.finished.load();
        if (finished && out.seconds == 0) out.seconds = now;

        double target;
        const uint64_t entries = prog.entries();
        if (finished) {
            target = 1.0;
        } else if (job.expected > 0) {
            target = std::min(0.985, static_cast<double>(entries) / static_cast<double>(job.expected));
        } else {
            const double done = static_cast<double>(prog.dirs_done.load());
            const double queued = static_cast<double>(std::max<uint64_t>(1, prog.dirs_queued.load()));
            // Folders finished / found: converges to 1 as discovery slows down.
            target = std::min(0.97, 0.15 * std::min(1.0, now / 1.5) + 0.85 * (done / queued) * std::min(1.0, now / 0.6));
        }
        shown = std::max(shown, shown + (target - shown) * std::min(1.0, dt * (finished ? 6.0 : 3.0)));
        if (finished && target - shown < 0.004) shown = 1.0;
        if (shown >= 1.0 && done_at < 0) done_at = now;

        if (now - last_sample >= 0.25) {
            const double inst = static_cast<double>(entries - last_entries) / std::max(1e-3, now - last_sample);
            rate = history.empty() ? inst : rate * 0.55 + inst * 0.45;
            if (!finished) {
                history.push_back(rate);
                if (history.size() > 24) history.erase(history.begin());
            }
            last_entries = entries;
            last_sample = now;
        }

        IndexFrame f;
        f.time = now;
        f.progress = shown;
        f.done = done_at >= 0 && result != nullptr;
        f.flash = f.done ? std::sin(3.14159 * std::clamp((now - done_at) / 1.1, 0.0, 1.0)) : 0.0;
        f.entries = entries;
        f.files = prog.files.load();
        f.dirs = prog.dirs.load();
        f.errors = prog.errors.load();
        f.rate = f.done ? static_cast<double>(entries) / std::max(1e-3, out.seconds) : rate;
        f.rate_history = history;
        f.current = prog.current();
        f.subtitle = subtitle;
        if (f.done) {
            f.title = "Indexed " + fmt::thousands(entries) + " items in " + fmt::duration(out.seconds);
            f.footer = done_hint;
        } else {
            f.title = job.title;
            f.footer = fmt::duration(now) + " elapsed";
            if (!job.hardware_note.empty()) f.footer += "  ·  " + job.hardware_note;
            f.footer += "  ·  esc to cancel";
        }
        if (f.errors > 0)
            f.footer += "  ·  " + fmt::thousands(f.errors) + " folder" + (f.errors == 1 ? "" : "s") + " skipped";

        int cols, rows;
        t.size(cols, rows);
        if (cols != cur.width() || rows != cur.height()) {
            cur.resize(cols, rows);
            full = true;
        }
        draw_index_frame(cur, f, color);
        t.write(renderer.diff(prev, cur, full));
        full = false;
        std::swap(prev, cur);

        if (finished && !result) break;  // cancelled
        if (done_at >= 0 && (now - done_at >= linger || quit_early)) break;

        term::Event ev;
        if (t.poll(ev, 33)) {
            do {
                if (ev.type == term::Event::Type::Resize) full = true;
                if (ev.type != term::Event::Type::Key) continue;
                const bool stop = ev.key == term::Key::Escape || (ev.ctrl && ev.ch == U'c') ||
                                  (ev.key == term::Key::Char && ev.ch == U'q');
                if (done_at >= 0) quit_early = true;
                else if (stop) prog.cancel = true;
            } while (t.poll(ev, 0));
        }
    }
    worker.join();
    out.store = result;
    out.cancelled = !result;
    out.errors = prog.errors.load();
    if (out.seconds == 0) out.seconds = seconds_since(t0);
    return out;
}

IndexOutcome run_index_plain(const IndexJob& job, bool quiet, term::ColorMode mode) {
    IndexOutcome out;
    scan::Progress prog;
    std::shared_ptr<engine::IndexStore> result;
    const auto t0 = Clock::now();
    std::thread worker([&] { result = scan::build(job.options, prog, fmt::now_ms()); });
    const bool live = !quiet && term::stderr_is_tty();
    while (!prog.finished.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (live) {
            double secs = seconds_since(t0);
            std::fprintf(stderr, "\r  %s indexing  %s items  (%s/s)   ",
                         paint(">", fg(theme::kLime), mode).c_str(), fmt::thousands(prog.entries()).c_str(),
                         fmt::compact(static_cast<uint64_t>(prog.entries() / std::max(0.1, secs))).c_str());
            std::fflush(stderr);
        }
    }
    worker.join();
    out.store = result;
    out.cancelled = !result;
    out.errors = prog.errors.load();
    out.seconds = seconds_since(t0);
    if (!quiet) {
        std::fprintf(stderr, "%s%s Indexed %s items in %s\n", live ? "\r\x1b[2K" : "",
                     paint(glyphs().check, fg(theme::kOk, kBold), mode).c_str(), fmt::thousands(prog.entries()).c_str(),
                     fmt::duration(out.seconds).c_str());
    }
    return out;
}

void run_pear_demo(term::Terminal& t, term::ColorMode mode, double seconds) {
    Renderer renderer(mode);
    Canvas prev, cur;
    bool full = true;
    const auto t0 = Clock::now();
    const bool color = mode != term::ColorMode::None;
    std::vector<double> history;
    for (;;) {
        const double now = seconds_since(t0);
        const double cycle = std::fmod(now, seconds + 2.5);
        const double p = std::clamp(cycle / seconds, 0.0, 1.0);
        const double eased = p < 0.5 ? 2 * p * p : 1 - std::pow(-2 * p + 2, 2) / 2;
        IndexFrame f;
        f.time = now;
        f.progress = eased;
        f.done = p >= 1.0;
        f.flash = f.done ? std::sin(3.14159 * std::clamp((cycle - seconds) / 1.1, 0.0, 1.0)) : 0.0;
        f.entries = static_cast<uint64_t>(eased * 412331);
        f.dirs = static_cast<uint64_t>(eased * 38201);
        f.rate = 140000 + 30000 * std::sin(now * 2.0);
        if (history.size() < 24 || static_cast<int>(now * 4) != static_cast<int>((now - 0.034) * 4)) {
            history.push_back(f.rate);
            if (history.size() > 24) history.erase(history.begin());
        }
        f.rate_history = history;
        f.current = std::string("~") + fileio::kPathSep + "Documents" + fileio::kPathSep + "Projects";
        f.subtitle = "Demo  ·  press any key to exit";
        f.title = f.done ? "Indexed 412,331 items in 2.9 s" : "Indexing your files";
        f.footer = "pearch " + std::string(f.done ? "is ready" : "is warming up");
        int cols, rows;
        t.size(cols, rows);
        if (cols != cur.width() || rows != cur.height()) {
            cur.resize(cols, rows);
            full = true;
        }
        draw_index_frame(cur, f, color);
        t.write(renderer.diff(prev, cur, full));
        full = false;
        std::swap(prev, cur);
        term::Event ev;
        if (t.poll(ev, 33)) {
            if (ev.type == term::Event::Type::Key) return;
            if (ev.type == term::Event::Type::Resize) full = true;
        }
    }
}

}  // namespace tui
