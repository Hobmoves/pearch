#include "tui/search_view.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

#include "core/fileio.h"
#include "core/format.h"
#include "core/text.h"
#include "engine/matcher.h"
#include "platform/shell.h"
#include "tui/screen.h"
#include "tui/glyphs.h"
#include "tui/theme.h"
#include "tui/widgets.h"

namespace tui {

namespace {

using Clock = std::chrono::steady_clock;
using engine::TypeFilter;
using engine::SortMode;

const char* const kTabs[] = {"All", "Files", "Folders", "Apps"};

std::u32string to_u32(std::string_view s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) out.push_back(text::decode_utf8(s, i));
    return out;
}

std::string to_u8(const std::u32string& s) {
    std::string out;
    for (char32_t c : s) text::append_utf8(out, c);
    return out;
}

bool is_word_char(char32_t c) {
    return c != U' ' && c != U'\\' && c != U'/' && c != U'.' && c != U'_' && c != U'-';
}

std::string base64(std::string_view in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
        out += tbl[v >> 18]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += tbl[v & 63];
    }
    if (i < in.size()) {
        uint32_t v = uint8_t(in[i]) << 16;
        if (i + 1 < in.size()) v |= uint8_t(in[i + 1]) << 8;
        out += tbl[v >> 18]; out += tbl[(v >> 12) & 63];
        out += i + 1 < in.size() ? tbl[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

class SearchView {
public:
    SearchView(term::Terminal& t, term::ColorMode mode, SearchSession& s)
        : t_(t), mode_(mode), color_(mode != term::ColorMode::None), s_(s), renderer_(mode),
          async_(pool_, [&t] { t.wake(); }) {
        query_ = to_u32(s.query);
        cursor_ = query_.size();
        store_ = s.store;
        resolve_boosts();
    }

    ~SearchView() {
        if (refresh_thread_.joinable()) {
            if (refresh_progress_) refresh_progress_->cancel = true;
            refresh_thread_.join();
        }
    }

    int run() {
        submit();
        if (s_.refresh_now) start_refresh();
        for (;;) {
            if (auto r = async_.take()) accept(std::move(r));
            finish_refresh_if_ready();
            draw();
            if (quit_) break;
            const bool animating = async_.busy() || refreshing_ || toast_active() || pending_draw_;
            pending_draw_ = false;
            term::Event ev;
            if (!t_.poll(ev, animating ? 50 : 1000)) continue;
            do {
                handle(ev);
            } while (!quit_ && t_.poll(ev, 0));
            if (quit_) break;
        }
        return 0;
    }

private:
    // ---- searching ----
    void resolve_boosts() {
        if (s_.history && store_) boosts_ = s_.history->resolve(*store_, fmt::now_ms());
    }

    void submit() {
        engine::SearchRequest req;
        req.text = to_u8(query_);
        req.tab = static_cast<TypeFilter>(tab_);
        req.sort = sort_;
        req.scope = s_.scope;
        req.now_ms = fmt::now_ms();
        req.boosts = boosts_;
        submitted_at_ = Clock::now();
        async_.submit(store_, std::move(req));
    }

    void accept(std::unique_ptr<engine::SearchResult> r) {
        const bool same_query = result_ && result_->query.raw == r->query.raw &&
                                result_->query.type == r->query.type && result_->sort == r->sort;
        uint32_t keep = engine::kNone;
        if (same_query && result_ && sel_ < result_->size()) keep = result_->id(sel_);
        result_ = std::move(r);
        if (!same_query) {
            sel_ = 0;
            scroll_ = 0;
        } else if (keep != engine::kNone && result_->store == store_) {
            sel_ = 0;
            for (size_t k = 0; k < std::min<size_t>(result_->size(), 500); ++k)
                if (result_->id(k) == keep) { sel_ = k; break; }
        }
        clamp_selection();
    }

    void clamp_selection() {
        size_t n = result_ ? result_->size() : 0;
        if (n == 0) { sel_ = 0; scroll_ = 0; return; }
        if (sel_ >= n) sel_ = n - 1;
        int rows = std::max(1, list_rows_);
        if (sel_ < scroll_) scroll_ = sel_;
        if (sel_ >= scroll_ + static_cast<size_t>(rows)) scroll_ = sel_ - rows + 1;
    }

    // ---- background refresh ----
    void start_refresh() {
        if (refreshing_ || !s_.rebuild) return;
        refreshing_ = true;
        refresh_done_ = false;
        refresh_progress_ = std::make_unique<scan::Progress>();
        refresh_started_ = Clock::now();
        refresh_thread_ = std::thread([this] {
            refresh_result_ = s_.rebuild(*refresh_progress_);
            refresh_done_ = true;
            t_.wake();
        });
    }

    void finish_refresh_if_ready() {
        if (!refreshing_ || !refresh_done_.load()) return;
        refresh_thread_.join();
        refreshing_ = false;
        if (refresh_result_) {
            const int64_t delta = static_cast<int64_t>(refresh_result_->size()) - static_cast<int64_t>(store_->size());
            store_ = refresh_result_;
            if (s_.on_rebuilt) s_.on_rebuilt(*refresh_result_);
            refresh_result_.reset();
            resolve_boosts();
            submit();
            std::string msg = "Index up to date";
            if (delta > 0) msg += "  ·  " + fmt::thousands(static_cast<uint64_t>(delta)) + " new";
            toast(msg, true);
        }
        refresh_progress_.reset();
    }

    // ---- input ----
    void edited() {
        submit();
    }

    void handle(const term::Event& ev) {
        using term::Key;
        if (ev.type == term::Event::Type::Resize) { full_ = true; return; }
        if (ev.type == term::Event::Type::Wake) return;
        if (ev.type == term::Event::Type::Mouse) { mouse(ev); return; }

        const size_t n = result_ ? result_->size() : 0;
        const int page = std::max(1, list_rows_ - 1);
        if (ev.key == Key::Char && (ev.ctrl || ev.alt)) {
            switch (ev.ch) {
                case U'c': case U'q': case U'd': quit_ = true; return;
                case U'u': query_.clear(); cursor_ = 0; edited(); return;
                case U'w': delete_word(); return;
                case U'a': cursor_ = 0; return;
                case U'e': cursor_ = query_.size(); return;
                case U'n': move(1); return;
                case U'p': move(-1); return;
                case U'o': act_reveal(); return;
                case U'y': act_copy(); return;
                case U's': sort_ = static_cast<SortMode>((static_cast<int>(sort_) + 1) % 4); edited(); return;
                case U'r': start_refresh(); return;
                case U'l': full_ = true; return;
                default: return;
            }
        }
        switch (ev.key) {
            case Key::Char:
                query_.insert(query_.begin() + static_cast<std::ptrdiff_t>(cursor_), ev.ch);
                ++cursor_;
                edited();
                return;
            case Key::Backspace:
                if (ev.ctrl || ev.alt) { delete_word(); return; }
                if (cursor_ > 0) {
                    query_.erase(cursor_ - 1, 1);
                    --cursor_;
                    edited();
                }
                return;
            case Key::Delete:
                if (cursor_ < query_.size()) { query_.erase(cursor_, 1); edited(); }
                return;
            case Key::Left:
                if (ev.ctrl) { while (cursor_ > 0 && !is_word_char(query_[cursor_ - 1])) --cursor_;
                               while (cursor_ > 0 && is_word_char(query_[cursor_ - 1])) --cursor_; }
                else if (cursor_ > 0) --cursor_;
                return;
            case Key::Right:
                if (ev.ctrl) { while (cursor_ < query_.size() && !is_word_char(query_[cursor_])) ++cursor_;
                               while (cursor_ < query_.size() && is_word_char(query_[cursor_])) ++cursor_; }
                else if (cursor_ < query_.size()) ++cursor_;
                return;
            case Key::Home: if (ev.ctrl) { sel_ = 0; clamp_selection(); } else cursor_ = 0; return;
            case Key::End: if (ev.ctrl) { sel_ = n ? n - 1 : 0; clamp_selection(); } else cursor_ = query_.size(); return;
            case Key::Up: move(-1); return;
            case Key::Down: move(1); return;
            case Key::PageUp: move(-page); return;
            case Key::PageDown: move(page); return;
            case Key::Tab: tab_ = (tab_ + 1) % 4; edited(); return;
            case Key::BackTab: tab_ = (tab_ + 3) % 4; edited(); return;
            case Key::Enter: act_open(ev.ctrl || ev.alt); return;
            case Key::Escape:
                if (!query_.empty()) { query_.clear(); cursor_ = 0; edited(); }
                else if (tab_ != 0) { tab_ = 0; edited(); }
                else quit_ = true;
                return;
            default: return;
        }
    }

    void delete_word() {
        size_t start = cursor_;
        while (start > 0 && !is_word_char(query_[start - 1])) --start;
        while (start > 0 && is_word_char(query_[start - 1])) --start;
        if (start == cursor_) return;
        query_.erase(start, cursor_ - start);
        cursor_ = start;
        edited();
    }

    void move(int delta) {
        size_t n = result_ ? result_->size() : 0;
        if (n == 0) return;
        long long v = static_cast<long long>(sel_) + delta;
        sel_ = static_cast<size_t>(std::clamp<long long>(v, 0, static_cast<long long>(n) - 1));
        clamp_selection();
    }

    void mouse(const term::Event& ev) {
        using term::MouseKind;
        if (ev.mouse == MouseKind::WheelUp) { move(-3); return; }
        if (ev.mouse == MouseKind::WheelDown) { move(3); return; }
        if (ev.mouse != MouseKind::Press) return;
        if (ev.y == tabs_y_) {
            for (int i = 0; i < 4; ++i)
                if (ev.x >= tab_x_[i] && ev.x < tab_x_[i + 1]) { tab_ = i; edited(); }
            return;
        }
        if (ev.y >= list_y_ && ev.y < list_y_ + list_rows_ && ev.x < list_w_ + 1) {
            size_t row = scroll_ + static_cast<size_t>(ev.y - list_y_);
            if (!result_ || row >= result_->size()) return;
            auto now = Clock::now();
            bool dbl = row == last_click_row_ && now - last_click_ < std::chrono::milliseconds(450);
            sel_ = row;
            clamp_selection();
            last_click_ = now;
            last_click_row_ = row;
            if (dbl) act_open(ev.ctrl);
        }
    }

    // ---- actions ----
    bool selected(uint32_t& id) {
        if (!result_ || result_->empty() || sel_ >= result_->size()) return false;
        id = result_->id(sel_);
        return true;
    }

    void remember(const std::string& path) {
        if (!s_.history) return;
        s_.history->record(path, fmt::now_ms());
        if (s_.save_history) s_.save_history();
    }

    void act_open(bool stay) {
        uint32_t id;
        if (!selected(id)) return;
        std::string path = result_->store->path(id);
        if (!shell::open(path)) { toast("Couldn't open " + std::string(result_->store->name(id)), false); return; }
        remember(path);
        if (stay) { resolve_boosts(); toast("Opened " + std::string(theme::display_name(*result_->store, id)), true); }
        else quit_ = true;
    }

    void act_reveal() {
        uint32_t id;
        if (!selected(id)) return;
        std::string path = result_->store->path(id);
        if (!shell::reveal(path)) { toast("Couldn't open the folder", false); return; }
        remember(path);
        quit_ = true;
    }

    void act_copy() {
        uint32_t id;
        if (!selected(id)) return;
        std::string path = result_->store->path(id);
        if (shell::copy_text(path)) {
            toast("Copied  " + path, true);
        } else {
            t_.write("\x1b]52;c;" + base64(path) + "\x07");  // ask the terminal to copy (OSC 52)
            toast("Copied  " + path, true);
        }
    }

    void toast(std::string msg, bool ok) {
        toast_ = std::move(msg);
        toast_ok_ = ok;
        toast_until_ = Clock::now() + std::chrono::milliseconds(2200);
    }
    bool toast_active() const { return !toast_.empty() && Clock::now() < toast_until_; }

    // ---- drawing ----
    Style S(uint32_t color, uint8_t attr = 0, uint32_t bg = kDefault) const {
        if (!color_) return Style{kDefault, kDefault, attr};
        return Style{color, bg, attr};
    }

    std::string pretty_dir(uint32_t id) const {
        std::string p = result_->store->parent_path(id);
        if (!s_.home.empty() && fileio::is_within(p, s_.home)) return "~" + p.substr(s_.home.size());
        return p;
    }

    void draw() {
        int W, H;
        t_.size(W, H);
        if (W != cur_.width() || H != cur_.height()) {
            cur_.resize(W, H);
            full_ = true;
        }
        cur_.clear();
        const double now = std::chrono::duration<double>(Clock::now() - born_).count();
        const bool roomy = H >= 14;
        int y = roomy ? 1 : 0;

        // Header: title, index size, refresh state.
        int x = 2;
        x += draw_small_wordmark(cur_, x, y, color_, false) + 2;
        if (store_) {
            std::string info = fmt::thousands(store_->size()) + " items";
            cur_.text(x, y, info, S(theme::kFaint));
            x += text::display_width(info) + 2;
        }
        if (!s_.scope_label.empty()) {
            std::string chip = " in " + s_.scope_label + " ";
            cur_.text(x, y, text::truncate_middle(chip, std::max(8, W / 2 - x)), S(theme::kGold, 0, color_ ? 0x2E2A1E : kDefault));
        }
        std::string right;
        uint32_t right_color = theme::kFaint;
        if (refreshing_ && refresh_progress_) {
            std::string sp;
            text::append_utf8(sp, spinner(now));
            right = sp + " refreshing · " + fmt::compact(refresh_progress_->entries());
            right_color = theme::kLime;
        } else if (store_ && store_->built_at() > 0) {
            right = "updated " + fmt::relative_time(store_->built_at(), fmt::now_ms());
        }
        if (!right.empty()) {
            int rw = text::display_width(right);
            cur_.text(W - 2 - rw, y, right, S(right_color));
        }
        y += 1;

        // Search box.
        const int bx = 1, bw = W - 2;
        cur_.box(bx, y, bw, 3, S(query_.empty() ? theme::kBorder : theme::kBorderFocus));
        const int iy = y + 1;
        cur_.text(bx + 2, iy, glyphs().prompt, S(theme::kLime, kBold));
        std::string count;
        if (result_) {
            if (result_->recent && query_.empty()) count = "";
            else {
                count = fmt::thousands(result_->size()) + (result_->size() == 1 ? " result" : " results") + " · " +
                        fmt::query_time(result_->ms);
            }
        }
        if (async_.busy() && Clock::now() - submitted_at_ > std::chrono::milliseconds(120)) count = "searching…";
        const int count_w = text::display_width(count);
        const int input_x = bx + 5;
        const int input_w = std::max(4, bw - 7 - (count_w ? count_w + 3 : 0));
        std::string q = to_u8(query_);
        int caret_col = 0;
        if (query_.empty()) {
            cur_.text(input_x, iy, s_.scope_label.empty() ? "Search files, folders and apps" : "Search in " + s_.scope_label,
                      S(theme::kFaint, kItalic), input_w);
        } else {
            // Keep the caret visible when the query is longer than the box.
            std::u32string before = query_.substr(0, cursor_);
            int before_w = text::display_width(to_u8(before));
            size_t start = 0;
            while (before_w - text::display_width(to_u8(query_.substr(0, start))) > input_w - 1 && start < cursor_) ++start;
            std::string visible = to_u8(query_.substr(start));
            cur_.text(input_x, iy, visible, S(theme::kText, kBold), input_w);
            caret_col = text::display_width(to_u8(query_.substr(start, cursor_ - start)));
        }
        if (count_w) cur_.text(bx + bw - 2 - count_w, iy, count, S(theme::kMuted));
        caret_x_ = input_x + caret_col;
        caret_y_ = iy;
        y += 3;

        // Tabs + sort.
        tabs_y_ = y;
        x = 2;
        for (int i = 0; i < 4; ++i) {
            tab_x_[i] = x;
            std::string label = std::string(" ") + kTabs[i] + " ";
            if (i == tab_) {
                cur_.text(x, y, label, color_ ? Style{0x14170F, theme::kLime, kBold} : Style{kDefault, kDefault, kReverse});
            } else {
                cur_.text(x, y, label, S(theme::kMuted));
            }
            x += text::display_width(label) + 1;
        }
        tab_x_[4] = x;
        std::string sort_label;
        if (result_ && result_->recent)
            sort_label = tab_ == 3 ? "all apps" : "recent";
        else
            sort_label = std::string("sorted by ") + engine::sort_mode_name(result_ ? result_->sort : sort_);
        if (result_ && result_->fuzzy) sort_label = "incl. close matches  ·  " + sort_label;
        cur_.text(W - 2 - text::display_width(sort_label), y, sort_label, S(theme::kFaint));
        y += roomy ? 2 : 1;

        // Results (+ details panel on wide screens).
        const int footer_h = 2;
        list_y_ = y;
        list_rows_ = std::max(0, H - y - footer_h);
        const bool details = W >= 118 && list_rows_ >= 8;
        const int panel_w = details ? 40 : 0;
        list_w_ = W - 2 - (details ? panel_w + 2 : 0);
        clamp_selection();
        draw_results(1, list_y_, list_w_, list_rows_);
        if (details) {
            for (int r = 0; r < list_rows_; ++r) cur_.put(W - panel_w - 2, list_y_ + r, U'│', S(theme::kBorder));
            draw_details(W - panel_w, list_y_, panel_w - 1, list_rows_);
        }

        // Footer.
        const int fy = H - 1;
        for (int i = 1; i < W - 1; ++i) cur_.put(i, fy - 1, U'─', S(theme::kBorder));
        if (toast_active()) {
            cur_.text(2, fy, toast_ok_ ? glyphs().check : "!", S(toast_ok_ ? theme::kOk : theme::kError, kBold));
            cur_.text(4, fy, text::truncate_middle(toast_, W - 6), S(toast_ok_ ? theme::kText : theme::kError));
        } else {
            draw_hints(2, fy, W - 4);
        }

        std::string frame = "\x1b[?25l" + renderer_.diff(prev_, cur_, full_);
        frame += "\x1b[" + std::to_string(caret_y_ + 1) + ";" + std::to_string(caret_x_ + 1) + "H\x1b[?25h";
        t_.write(frame);
        full_ = false;
        std::swap(prev_, cur_);
    }

    void draw_hints(int x, int y, int w) {
        struct Hint { const char* key; const char* label; };
        const Hint hints[] = {
            {"↑↓", "move"}, {glyphs().enter, "open"}, {"^O", "show in folder"}, {"^Y", "copy path"},
            {"tab", "type"}, {"^S", "sort"}, {"esc", query_.empty() ? "quit" : "clear"},
        };
        int used = 0;
        for (const auto& h : hints) {
            int need = text::display_width(h.key) + 1 + text::display_width(h.label) + 3;
            if (used + need > w) break;
            used += cur_.text(x + used, y, h.key, S(theme::kLime, kBold)) + 1;
            used += cur_.text(x + used, y, h.label, S(theme::kFaint)) + 3;
        }
    }

    void draw_results(int x, int y, int w, int rows) {
        if (rows <= 0) return;
        if (!result_) {
            center(x, y, w, rows, "Loading…", "");
            return;
        }
        if (result_->empty()) {
            if (async_.busy()) return;
            if (query_.empty() && result_->recent) {
                center(x, y, w, rows, "Start typing to search " + fmt::thousands(store_ ? store_->size() : 0) + " items",
                       "words in any order  ·  *.pdf  ·  ext:docx  ·  in:Documents  ·  modified:week");
            } else {
                center(x, y, w, rows, "No matches for “" + to_u8(query_) + "”",
                       "try fewer letters  ·  check the Files / Folders / Apps tab  ·  ctrl+r to refresh");
            }
            return;
        }
        const engine::IndexStore& st = *result_->store;
        const int64_t now = fmt::now_ms();
        const bool show_meta = w >= 72;
        const bool show_path = w >= 44;
        const int date_w = show_meta ? 11 : 0;
        const int size_w = show_meta ? 9 : 0;
        const int name_x = x + 8;
        const int rest = w - 8 - date_w - size_w;
        const int name_w = show_path ? std::max(12, rest * 46 / 100) : rest;
        const int path_x = name_x + name_w + 2;
        const int path_w = std::max(0, rest - name_w - 3);

        for (int r = 0; r < rows; ++r) {
            const size_t k = scroll_ + static_cast<size_t>(r);
            if (k >= result_->size()) break;
            const uint32_t id = result_->id(k);
            const bool sel = k == sel_;
            const int ry = y + r;
            const uint32_t bg = sel && color_ ? theme::kSelBg : kDefault;
            if (sel) {
                if (color_) cur_.fill_bg(x, ry, w, 1, bg);
                cur_.put(x, ry, U'▌', color_ ? Style{theme::kLime, bg, 0} : Style{kDefault, kDefault, kBold});
            }
            auto badge = theme::badge(st, id);
            cur_.text(x + 2, ry, badge.label, color_ ? Style{badge.color, bg, kBold} : Style{kDefault, kDefault, kDim});

            // Name with matched characters highlighted.
            std::string_view name = theme::display_name(st, id);
            std::vector<uint8_t> marks = engine::match::highlight(result_->query, st.name(id), result_->fuzzy);
            draw_name(name_x, ry, name, marks, name_w, sel, bg);

            if (show_path && path_w > 4) {
                cur_.text(path_x, ry, text::truncate_path(pretty_dir(id), path_w, fileio::kPathSep),
                          color_ ? Style{sel ? theme::kMuted : theme::kFaint, bg, 0} : Style{kDefault, kDefault, kDim});
            }
            if (show_meta) {
                std::string size = st.is_dir(id) ? "" : fmt::size(st.file_size(id));
                std::string date = fmt::relative_time(st.mtime(id), now);
                cur_.text(x + w - date_w - size_w + (size_w - 1 - text::display_width(size)), ry, size,
                          color_ ? Style{theme::kMuted, bg, 0} : Style{});
                cur_.text(x + w - date_w + (date_w - 1 - text::display_width(date)), ry, date,
                          color_ ? Style{theme::kFaint, bg, 0} : Style{kDefault, kDefault, kDim});
            }
        }
        // Scroll indicator.
        if (result_->size() > static_cast<size_t>(rows) && rows >= 3) {
            const double frac = static_cast<double>(scroll_) / static_cast<double>(result_->size() - rows);
            const int pos = static_cast<int>(frac * (rows - 1) + 0.5);
            for (int r = 0; r < rows; ++r)
                cur_.put(x + w, y + r, r == pos ? glyphs().thumb : U'│', S(r == pos ? theme::kMuted : 0x2A303C));
        }
    }

    void draw_name(int x, int y, std::string_view name, const std::vector<uint8_t>& marks, int max_w, bool sel,
                   uint32_t bg) {
        const int total = text::display_width(name);
        const bool cut = total > max_w;
        const int limit = cut ? max_w - 1 : max_w;
        int used = 0;
        size_t cp = 0;
        for (size_t i = 0; i < name.size(); ++cp) {
            char32_t ch = text::decode_utf8(name, i);
            int cw = text::char_width(ch);
            if (cw == 0) continue;
            if (used + cw > limit) break;
            const bool hit = cp < marks.size() && marks[cp];
            Style st;
            if (color_) st = Style{hit ? theme::kMatch : (sel ? 0xFFFFFF : theme::kText), bg,
                                   static_cast<uint8_t>(hit || sel ? kBold : 0)};
            else st = Style{kDefault, kDefault, static_cast<uint8_t>(hit ? kUnderline | kBold : (sel ? kBold : 0))};
            cur_.put(x + used, y, ch < 0x20 ? U'?' : ch, st);
            used += cw;
        }
        if (cut) cur_.put(x + used, y, U'…', S(theme::kFaint, 0, bg));
    }

    void center(int x, int y, int w, int rows, const std::string& line1, const std::string& line2) {
        int cy = y + std::max(0, rows / 2 - 2);
        cur_.text(x + std::max(0, (w - text::display_width(line1)) / 2), cy, text::truncate_end(line1, w), S(theme::kMuted, kBold));
        if (!line2.empty())
            cur_.text(x + std::max(0, (w - text::display_width(line2)) / 2), cy + 2, text::truncate_end(line2, w),
                      S(theme::kFaint));
    }

    void draw_details(int x, int y, int w, int rows) {
        uint32_t id;
        if (!selected(id)) return;
        const engine::IndexStore& st = *result_->store;
        auto badge = theme::badge(st, id);
        int yy = y;
        cur_.text(x, yy, " " + badge.label + " ", color_ ? Style{0x111318, badge.color, kBold} : Style{kDefault, kDefault, kReverse});
        yy += 2;
        // Name, wrapped over up to 3 lines.
        std::string name(theme::display_name(st, id));
        for (int line = 0; line < 3 && !name.empty(); ++line) {
            std::string part;
            int used = 0;
            size_t i = 0;
            while (i < name.size()) {
                size_t start = i;
                char32_t ch = text::decode_utf8(name, i);
                int cw = text::char_width(ch);
                if (used + cw > w) { i = start; break; }
                part.append(name, start, i - start);
                used += cw;
            }
            name.erase(0, i);
            if (line == 2 && !name.empty()) part = text::truncate_end(part + name, w);
            cur_.text(x, yy++, part, S(theme::kText, kBold));
        }
        yy += 1;
        auto row = [&](const char* label, const std::string& value, uint32_t color) {
            if (yy >= y + rows - 1) return;
            cur_.text(x, yy, label, S(theme::kFaint));
            cur_.text(x + 10, yy, text::truncate_end(value, w - 10), S(color));
            ++yy;
        };
        const uint8_t f = st.flags(id);
        std::string kind = (f & engine::kApp) ? "App shortcut" : (f & engine::kDir) ? "Folder" : badge.label + " file";
        if (f & engine::kHidden) kind += " (hidden)";
        row("Type", kind, theme::kMuted);
        if (f & engine::kDir) {
            uint32_t inside = st.subtree_end(id) - id - 1;
            row("Contains", fmt::thousands(inside) + " items", theme::kMuted);
        } else {
            row("Size", fmt::size(st.file_size(id)) + "  (" + fmt::thousands(st.file_size(id)) + " bytes)", theme::kMuted);
        }
        row("Modified", fmt::date_time(st.mtime(id)), theme::kMuted);
        row("", fmt::relative_time(st.mtime(id), fmt::now_ms()), theme::kFaint);
        yy += 1;
        if (yy < y + rows - 1) cur_.text(x, yy++, "Location", S(theme::kFaint));
        std::string loc = pretty_dir(id);
        while (!loc.empty() && yy < y + rows - 2) {
            if (text::display_width(loc) <= w) { cur_.text(x, yy++, loc, S(theme::kMuted)); break; }
            // Hard-wrap long paths at the panel width.
            size_t i = 0;
            int used = 0;
            while (i < loc.size()) {
                size_t start = i;
                int cw = text::char_width(text::decode_utf8(loc, i));
                if (used + cw > w) { i = start; break; }
                used += cw;
            }
            cur_.text(x, yy++, loc.substr(0, i), S(theme::kMuted));
            loc.erase(0, i);
        }
        int hy = y + rows - 1;
        int hx = x;
        hx += cur_.text(hx, hy, glyphs().enter, S(theme::kLime, kBold)) + 1;
        hx += cur_.text(hx, hy, "open", S(theme::kFaint)) + 3;
        hx += cur_.text(hx, hy, "^O", S(theme::kLime, kBold)) + 1;
        cur_.text(hx, hy, "show in folder", S(theme::kFaint));
    }

    term::Terminal& t_;
    term::ColorMode mode_;
    bool color_;
    SearchSession& s_;
    Renderer renderer_;
    engine::ThreadPool pool_;
    engine::AsyncSearcher async_;
    std::shared_ptr<const engine::IndexStore> store_;
    std::shared_ptr<const engine::Boosts> boosts_;
    std::unique_ptr<engine::SearchResult> result_;

    std::u32string query_;
    size_t cursor_ = 0;
    int tab_ = 0;
    SortMode sort_ = SortMode::Relevance;
    size_t sel_ = 0, scroll_ = 0;
    bool quit_ = false;
    bool full_ = true;
    bool pending_draw_ = false;

    Canvas prev_, cur_;
    int caret_x_ = 0, caret_y_ = 0;
    int list_y_ = 0, list_rows_ = 10, list_w_ = 80, tabs_y_ = -1;
    int tab_x_[5] = {0, 0, 0, 0, 0};
    Clock::time_point submitted_at_ = Clock::now();
    Clock::time_point born_ = Clock::now();
    Clock::time_point last_click_{};
    size_t last_click_row_ = static_cast<size_t>(-1);

    std::string toast_;
    bool toast_ok_ = true;
    Clock::time_point toast_until_{};

    bool refreshing_ = false;
    std::atomic<bool> refresh_done_{false};
    std::unique_ptr<scan::Progress> refresh_progress_;
    std::shared_ptr<engine::IndexStore> refresh_result_;
    std::thread refresh_thread_;
    Clock::time_point refresh_started_{};
};

}  // namespace

int run_search_view(term::Terminal& t, term::ColorMode mode, SearchSession& s) {
    SearchView view(t, mode, s);
    return view.run();
}

}  // namespace tui
