#include "app/commands.h"

#include <algorithm>
#include <cstdio>

#include "app/config.h"
#include "core/fileio.h"
#include "core/format.h"
#include "core/hardware.h"
#include "core/text.h"
#include "core/version.h"
#include "engine/history.h"
#include "engine/matcher.h"
#include "engine/searcher.h"
#include "platform/console.h"
#include "platform/shell.h"
#include "tui/index_view.h"
#include "tui/pear3d.h"
#include "tui/widgets.h"
#include "tui/screen.h"
#include "tui/search_view.h"
#include "tui/theme.h"

namespace app {

namespace {

using tui::fg;
using tui::paint;
namespace theme = tui::theme;

// ---- plain output helpers ----
struct Out {
    term::ColorMode mode;
    void raw(const std::string& s) const { std::fwrite(s.data(), 1, s.size(), stdout); }
    std::string c(std::string_view s, uint32_t color, uint8_t attr = 0) const {
        return paint(s, tui::Style{color, tui::kDefault, attr}, mode);
    }
    void line(const std::string& s = "") const { raw(s + "\n"); }
    void ok(const std::string& s) const { line("  " + c("✓", theme::kOk, tui::kBold) + " " + s); }
    void fail(const std::string& s) const { line("  " + c("✗", theme::kError, tui::kBold) + " " + s); }
};

void eprint(term::ColorMode mode, const std::string& msg) {
    std::string s = "  " + paint("✗", fg(theme::kError, tui::kBold), mode) + " " + msg + "\n";
    std::fwrite(s.data(), 1, s.size(), stderr);
}

std::string small_logo(const Out& o) {
    std::string s;
    const char* letters = "pearch";
    for (int i = 0; i < 6; ++i)
        s += o.c(std::string(1, letters[i]), tui::mix(theme::kLime, theme::kGold, i / 5.0f), tui::kBold);
    return s;
}

std::string pretty_path(const std::string& p) {
    std::string home = shell::home_dir();
    if (!home.empty() && fileio::is_within(p, home)) return "~" + p.substr(home.size());
    return p;
}

struct App {
    Paths paths = Paths::get();
    Config config;
    term::ColorMode mode;          // for full-screen and stderr
    term::ColorMode out_mode;      // for stdout (none when piped)
    Out out{term::ColorMode::None};

    explicit App(const Options& o) {
        term::prepare_output();
        mode = term::detect_color(o.no_color);
        out_mode = term::stdout_is_tty() ? mode : term::ColorMode::None;
        out.mode = out_mode;
        config = load_or_create(paths);
        hw::set_overrides(config.scan_threads, config.search_threads);
    }

    std::shared_ptr<engine::IndexStore> load_store(std::string* err = nullptr) const {
        return engine::IndexStore::load(paths.index, err);
    }

    bool save_store(const engine::IndexStore& s) const {
        std::string err;
        if (s.save(paths.index, &err)) return true;
        eprint(mode, "Could not save the index: " + err);
        return false;
    }

    std::vector<std::string> labels(const std::vector<std::string>& locs) const {
        std::vector<std::string> out_labels;
        for (const auto& l : locs) {
            std::string lab = config.label(l);
            if (std::find(out_labels.begin(), out_labels.end(), lab) == out_labels.end()) out_labels.push_back(lab);
        }
        return out_labels;
    }

    // Builds an index for `opt`, full screen when possible.
    tui::IndexOutcome build(const scan::Options& opt, uint64_t expected, const std::string& title,
                            bool interactive, bool quiet, const std::string& hint, double linger,
                            term::Terminal* t) {
        tui::IndexJob job;
        job.options = opt;
        if (job.options.threads == 0) {
            hw::ScanPlan plan = hw::scan_plan(opt.roots);
            job.options.threads = plan.threads;
            job.hardware_note = std::to_string(plan.threads) + " threads on " + hw::storage_name(plan.storage);
        }
        job.expected = expected;
        job.title = title;
        job.labels = labels(opt.roots);
        if (interactive && t && (t->active() || t->start(false))) {
            return tui::run_index_view(*t, mode, job, hint, linger);
        }
        return tui::run_index_plain(job, quiet, mode);
    }
};

bool interactive_terminal() { return term::stdout_is_tty() && term::stdin_is_tty(); }

// ---- commands ----

int cmd_help(const Options& o) {
    Out out{term::stdout_is_tty() ? term::detect_color(o.no_color) : term::ColorMode::None};
    auto h = [&](const char* s) { out.line("  " + out.c(s, theme::kGold, tui::kBold)); };
    auto row = [&](const std::string& cmd, const std::string& desc) {
        out.line("    " + out.c(text::pad_right(cmd, 26), theme::kLime) + out.c(desc, theme::kMuted));
    };
    out.line();
    out.line("  " + small_logo(out) + out.c("  " PEARCH_VERSION, theme::kFaint) + "   " +
             out.c("instant file search", theme::kMuted));
    out.line();
    h("USAGE");
    row("pearch", "open the search screen (just start typing)");
    row("pearch <words>", "open it with these words already typed");
    row("pearch -l <words>", "print matches instead (also when piped)");
    row("pearch --in <folder>", "search inside one folder");
    out.line();
    h("COMMANDS");
    row("index", "rebuild the index now");
    row("add <folder>", "also index a folder or drive (D:\\, ~/code)");
    row("remove <folder>", "stop indexing a folder");
    row("locations", "show what is indexed");
    row("stats", "index size, age and where it lives");
    row("install", "PATH + Start Menu + Ctrl+Alt+Space + Explorer menu");
    row("uninstall [--purge]", "undo install (--purge: delete the index too)");
    out.line();
    h("SEARCH SYNTAX");
    row("q3 report", "all words, any order (name or its folders)");
    row("\"q3 report\"", "exact phrase");
    row("*.pdf   report*.docx", "wildcards");
    row("ext:pdf,docx", "by extension");
    row("file:  folder:  app:", "only files / folders / apps");
    row("in:work    work\\q3", "inside a folder / by path");
    row("-draft", "leave out names with \"draft\"");
    row("size:>10mb", "by size (also empty, large, huge)");
    row("modified:week", "today, yesterday, week, month, year, 3d, 12h");
    out.line();
    h("KEYS");
    out.line("    " + out.c("↑↓", theme::kLime) + out.c(" move   ", theme::kMuted) + out.c("⏎", theme::kLime) +
             out.c(" open   ", theme::kMuted) + out.c("ctrl+⏎", theme::kLime) + out.c(" open & stay   ", theme::kMuted) +
             out.c("ctrl+o", theme::kLime) + out.c(" show in folder", theme::kMuted));
    out.line("    " + out.c("ctrl+y", theme::kLime) + out.c(" copy path   ", theme::kMuted) + out.c("tab", theme::kLime) +
             out.c(" files/folders/apps   ", theme::kMuted) + out.c("ctrl+s", theme::kLime) +
             out.c(" sort   ", theme::kMuted) + out.c("ctrl+r", theme::kLime) + out.c(" refresh", theme::kMuted));
    out.line("    " + out.c("esc", theme::kLime) + out.c(" clear, then quit   ", theme::kMuted) + out.c("mouse", theme::kLime) +
             out.c(" wheel scrolls, double-click opens", theme::kMuted));
    out.line();
    h("OPTIONS");
    row("-l, --list", "print results          -n, --limit N   at most N");
    row("--no-color", "plain output           -q, --quiet     less output");
    out.line();
    return 0;
}

// A short guided tour, shown after installing (and any time: `pearch tour`).
int cmd_tour(const Options& o) {
    term::prepare_output();
    const term::ColorMode mode = term::stdout_is_tty() ? term::detect_color(o.no_color) : term::ColorMode::None;
    Out out{mode};
    // A ripe pear next to the title, drawn by the same renderer as the indexing screen.
    tui::Canvas art(std::max(70, std::min(100, term::output_width() - 4)), 12);
    tui::PearParams pp;
    pp.time = 0.9;
    pp.progress = 1.0;
    pp.color = mode != term::ColorMode::None;
    tui::draw_pear(art, 0, 0, 22, 12, pp);
    tui::draw_small_wordmark(art, 26, 3, pp.color);
    auto label = [&](int y, const std::string& s, uint32_t c, uint8_t attr = 0) {
        art.text(26, y, s, pp.color ? tui::Style{c, tui::kDefault, attr} : tui::Style{tui::kDefault, tui::kDefault, attr});
    };
    label(5, "instant file search", theme::kText, tui::kBold);
    label(6, std::string("version ") + PEARCH_VERSION + "  ·  " + hw::summary(), theme::kFaint);
    label(8, "Here's the 30-second tour.", theme::kMuted);
    out.line();
    for (const auto& l : tui::to_lines(art, mode)) out.line("  " + l);
    out.line();

    auto step = [&](const char* n, const char* title) {
        out.line("  " + out.c(n, theme::kGold, tui::kBold) + "  " + out.c(title, theme::kText, tui::kBold));
    };
    auto ex = [&](const std::string& what, const std::string& desc) {
        out.line("       " + out.c(text::pad_right(what, 22), theme::kLime) + out.c(desc, theme::kMuted));
    };
    auto note = [&](const std::string& s) { out.line("       " + out.c(s, theme::kMuted)); };

#ifdef _WIN32
    step("1", "Open it");
    ex("Ctrl+Alt+Space", "from anywhere in Windows (after install)");
    ex("pearch", "from any terminal");
    note("The first launch indexes your files while the pear fills up.");
#else
    step("1", "Open it");
    ex("pearch", "from any terminal (bind it to a key in your desktop settings)");
    note("The first launch indexes your files while the pear fills up.");
#endif
    out.line();
    step("2", "Type");
    note("Results update on every key. Try:");
    ex("q3 report", "words in any order");
    ex("*.pdf", "every PDF, newest first");
    ex("vs code", "apps by name or initials");
    ex("in:downloads", "inside a folder");
    ex("modified:week", "changed this week");
    ex("budget -draft", "leave words out");
    out.line();
    step("3", "Act");
    out.line("       " + out.c("⏎", theme::kLime) + out.c(" open    ", theme::kMuted) + out.c("ctrl+o", theme::kLime) +
             out.c(" show in folder    ", theme::kMuted) + out.c("ctrl+y", theme::kLime) + out.c(" copy path", theme::kMuted));
    out.line("       " + out.c("tab", theme::kLime) + out.c(" files / folders / apps    ", theme::kMuted) +
             out.c("esc", theme::kLime) + out.c(" clear, then quit", theme::kMuted));
    out.line();
    step("4", "Make it yours");
#ifdef _WIN32
    ex("pearch add D:\\", "index another drive");
#else
    ex("pearch add ~/code", "index another folder or drive");
#endif
    ex("pearch stats", "what's indexed + how pearch uses your hardware");
    ex("pearch help", "everything else");
    out.line();
    return 0;
}

int cmd_version(const Options& o) {
    Out out{term::stdout_is_tty() ? term::detect_color(o.no_color) : term::ColorMode::None};
    out.line(small_logo(out) + " " PEARCH_VERSION);
    return 0;
}

int cmd_index(const Options& o) {
    App app(o);
    auto prev = app.load_store();
    const bool interactive = interactive_terminal() && !o.quiet && !o.list;
    term::Terminal t;
    auto r = app.build(app.config.scan_options(), prev ? prev->size() : 0,
                       prev ? "Refreshing your index" : "Indexing your files", interactive, o.quiet,
                       "press any key", 1.6, &t);
    t.stop();
    if (r.cancelled || !r.store) {
        eprint(app.mode, "Indexing cancelled; the previous index was kept.");
        return 1;
    }
    app.save_store(*r.store);
    if (!o.quiet && interactive) {
        app.out.line();
        app.out.ok("Indexed " + app.out.c(fmt::thousands(r.store->size()), theme::kText, tui::kBold) + " items in " +
                   fmt::duration(r.seconds) +
                   (r.errors ? app.out.c("  ·  " + fmt::thousands(r.errors) + " folders skipped (no access)", theme::kFaint) : ""));
        for (const auto& l : app.labels(app.config.locations)) app.out.line("    " + app.out.c(l, theme::kMuted));
        app.out.line();
    }
    return 0;
}

int cmd_add(const Options& o) {
    App app(o);
    const std::string abs = fileio::absolute(o.args[0]);
    if (!fileio::is_directory(abs)) {
        eprint(app.mode, "Not a folder: " + abs);
        return 1;
    }
    Config& cfg = app.config;
    for (const auto& loc : cfg.locations) {
        bool covered = fileio::is_within(abs, loc);
        for (const auto& sp : cfg.skip_paths) covered = covered && !fileio::is_within(abs, sp);
        if (covered) {
            app.out.ok(pretty_path(abs) + " is already indexed (inside " + pretty_path(loc) + ")");
            return 0;
        }
    }
    // Locations inside the new one become part of it.
    std::vector<std::string> absorbed;
    std::vector<std::string> kept;
    for (const auto& loc : cfg.locations) {
        if (fileio::is_within(loc, abs) && !std::count(cfg.app_paths.begin(), cfg.app_paths.end(), loc)) absorbed.push_back(loc);
        else kept.push_back(loc);
    }
    std::vector<std::string> nested_apps;
    for (const auto& loc : kept)
        if (fileio::is_within(loc, abs)) nested_apps.push_back(loc);
    kept.push_back(abs);
    cfg.locations = kept;
    cfg.save(app.paths.config);

    auto store = app.load_store();
    const bool interactive = interactive_terminal() && !o.quiet;
    term::Terminal t;
    if (!store) {
        auto r = app.build(cfg.scan_options(), 0, "Indexing your files", interactive, o.quiet, "press any key", 1.4, &t);
        t.stop();
        if (!r.store) { eprint(app.mode, "Indexing cancelled."); return 1; }
        app.save_store(*r.store);
        app.out.ok("Added " + pretty_path(abs) + "  ·  " + fmt::thousands(r.store->size()) + " items indexed");
        return 0;
    }
    scan::Options opt = cfg.scan_options();
    opt.roots = {abs};
    for (const auto& n : nested_apps) opt.skip_paths.push_back(n);  // indexed as their own location
    auto r = app.build(opt, 0, "Indexing " + pretty_path(abs), interactive, o.quiet, "press any key", 1.2, &t);
    t.stop();
    if (!r.store) { eprint(app.mode, "Indexing cancelled; nothing was added to the index."); return 1; }
    std::shared_ptr<engine::IndexStore> base = store;
    for (const auto& gone : absorbed) {
        uint32_t id = base->find_path(gone);
        if (id != engine::kNone && base->is_root(id)) base = base->without_root(id);
    }
    auto merged = engine::IndexStore::merge(*base, *r.store);
    if (merged) app.save_store(*merged);
    app.out.ok("Added " + app.out.c(pretty_path(abs), theme::kText, tui::kBold) + "  ·  " +
               fmt::thousands(r.store->size()) + " items in " + fmt::duration(r.seconds));
    return 0;
}

int cmd_remove(const Options& o) {
    App app(o);
    const std::string abs = fileio::absolute(o.args[0]);
    Config& cfg = app.config;
    auto it = std::find_if(cfg.locations.begin(), cfg.locations.end(),
                           [&](const std::string& l) { return fileio::is_within(l, abs) && fileio::is_within(abs, l); });
    if (it == cfg.locations.end()) {
        eprint(app.mode, pretty_path(abs) + " is not an indexed location. These are:");
        for (const auto& l : cfg.locations) app.out.line("    " + l);
        return 1;
    }
    cfg.locations.erase(it);
    cfg.save(app.paths.config);
    if (auto store = app.load_store()) {
        uint32_t id = store->find_path(abs);
        if (id != engine::kNone && store->is_root(id)) {
            auto smaller = store->without_root(id);
            if (smaller) app.save_store(*smaller);
        }
    }
    app.out.ok("Removed " + pretty_path(abs) + " from the index");
    return 0;
}

int cmd_locations(const Options& o) {
    App app(o);
    auto store = app.load_store();
    app.out.line();
    app.out.line("  " + app.out.c("Indexed locations", theme::kGold, tui::kBold));
    for (const auto& loc : app.config.locations) {
        std::string count;
        if (store) {
            uint32_t id = store->find_path(loc);
            if (id != engine::kNone && store->is_root(id))
                count = fmt::thousands(store->subtree_end(id) - id) + " items";
            else
                count = "not indexed yet";
        }
        app.out.line("    " + app.out.c("●", theme::kLime) + " " + text::pad_right(loc, 52) + " " +
                     app.out.c(count, theme::kFaint));
    }
    app.out.line();
    std::string skip;
    for (const auto& s : app.config.skip_names) skip += (skip.empty() ? "" : ", ") + s;
    app.out.line("  " + app.out.c("Never looked inside: ", theme::kFaint) + app.out.c(skip, theme::kMuted));
    for (const auto& s : app.config.skip_paths) app.out.line("  " + app.out.c("                      ", theme::kFaint) + app.out.c(s, theme::kMuted));
    app.out.line();
    app.out.line("  " + app.out.c("Change with ", theme::kFaint) + app.out.c("pearch add <folder>", theme::kLime) +
                 app.out.c(" / ", theme::kFaint) + app.out.c("pearch remove <folder>", theme::kLime) +
                 app.out.c(" or edit ", theme::kFaint) + app.out.c(app.paths.config, theme::kMuted));
    app.out.line();
    return 0;
}

int cmd_stats(const Options& o) {
    App app(o);
    std::string err;
    auto store = app.load_store(&err);
    Out& out = app.out;
    out.line();
    out.line("  " + small_logo(out) + out.c("  " PEARCH_VERSION, theme::kFaint));
    out.line();
    auto row = [&](const std::string& k, const std::string& v) {
        out.line("    " + out.c(text::pad_right(k, 12), theme::kFaint) + v);
    };
    if (!store) {
        row("index", out.c("none yet", theme::kMuted) + "  (run " + out.c("pearch", theme::kLime) + " to build it)");
    } else {
        uint64_t apps = 0;
        for (uint32_t i = 0; i < store->size(); ++i) apps += (store->flags(i) & engine::kApp) ? 1 : 0;
        row("items", out.c(fmt::thousands(store->size()), theme::kText, tui::kBold) + out.c("  (" +
            fmt::thousands(store->count_files()) + " files · " + fmt::thousands(store->count_dirs()) +
            " folders · " + fmt::thousands(apps) + " apps)", theme::kMuted));
        row("updated", fmt::relative_time(store->built_at(), fmt::now_ms()) + out.c("  (" + fmt::date_time(store->built_at()) + ")", theme::kFaint));
        row("on disk", fmt::size(fileio::file_size(app.paths.index)) + out.c("  ·  " + fmt::size(store->memory_bytes()) + " in memory", theme::kFaint));
    }
    row("locations", std::to_string(app.config.locations.size()) + out.c("  (pearch locations)", theme::kFaint));
    const hw::Info& h = hw::info();
    row("cpu", (h.cpu.empty() ? std::string("unknown") : h.cpu) + out.c("  (" + hw::summary() + ")", theme::kFaint));
    row("search", std::to_string(hw::search_threads()) + " threads" +
                      out.c(engine::match::simd_level_avx2() ? "  ·  AVX2 matching" : "  ·  SSE2 matching", theme::kFaint));
    hw::ScanPlan plan = hw::scan_plan(app.config.locations);
    row("indexing", std::to_string(plan.threads) + " threads" + out.c("  ·  tuned for " + std::string(hw::storage_name(plan.storage)), theme::kFaint));
    row("data", app.paths.dir);
    out.line();
    return 0;
}

int cmd_install(const Options& o, bool install) {
    App app(o);
    app.out.line();
    for (const auto& step : install ? shell::install() : shell::uninstall(o.purge)) {
        if (step.ok) app.out.ok(step.text);
        else app.out.fail(step.text);
    }
    if (install) {
#ifdef _WIN32
        app.out.line();
        app.out.line("  Press " + app.out.c("Ctrl+Alt+Space", theme::kLime, tui::kBold) +
                     " anywhere to search, or type " + app.out.c("pearch", theme::kLime, tui::kBold) +
                     " in a new terminal.");
#endif
    }
    app.out.line();
    return 0;
}

int cmd_scan(const Options& o) {
    App app(o);
    scan::Options opt = app.config.scan_options();
    opt.roots = {fileio::absolute(o.args[0])};
    scan::Progress p;
    auto t0 = fmt::now_ms();
    auto store = scan::build(opt, p, t0);
    double secs = static_cast<double>(fmt::now_ms() - t0) / 1000.0;
    app.out.line("Files: " + fmt::thousands(p.files.load()));
    app.out.line("Directories: " + fmt::thousands(p.dirs.load()));
    app.out.line("Errors: " + fmt::thousands(p.errors.load()));
    app.out.line("Time: " + fmt::duration(secs));
    return 0;
}

int cmd_pear(const Options& o) {
    term::prepare_output();
    term::Terminal t;
    if (!t.start(false)) return 1;
    tui::run_pear_demo(t, term::detect_color(o.no_color), 3.5);
    t.stop();
    return 0;
}

// Prints matches line by line (for -l and pipes).
int print_results(App& app, engine::SearchResult& r, const Options& o) {
    const bool pretty = term::stdout_is_tty();
    const size_t limit = o.limit ? o.limit : (pretty ? 20 : r.size());
    const size_t n = std::min(limit, r.size());
    const engine::IndexStore& st = *r.store;
    if (!pretty) {
        std::string buf;
        for (size_t k = 0; k < n; ++k) {
            buf += st.path(r.id(k));
            buf += '\n';
            if (buf.size() > (1 << 16)) { app.out.raw(buf); buf.clear(); }
        }
        app.out.raw(buf);
        return r.empty() ? 1 : 0;
    }
    Out& out = app.out;
    out.line();
    if (r.empty()) {
        out.line("  " + out.c("No matches for ", theme::kMuted) + out.c("“" + r.query.raw + "”", theme::kText, tui::kBold));
        out.line();
        return 1;
    }
    const int width = std::max(60, term::output_width());
    const int name_w = std::min(48, width / 2 - 8);
    for (size_t k = 0; k < n; ++k) {
        uint32_t id = r.id(k);
        auto badge = theme::badge(st, id);
        std::string line = "  " + out.c(text::pad_right(badge.label, 5), badge.color, tui::kBold) + " ";
        std::string_view name = theme::display_name(st, id);
        auto marks = engine::match::highlight(r.query, st.name(id), r.fuzzy);
        std::string shown = text::truncate_end(name, name_w);
        size_t cp = 0;
        std::string run;
        bool run_hit = false;
        auto flush = [&] {
            if (!run.empty()) line += out.c(run, run_hit ? theme::kMatch : theme::kText, run_hit ? tui::kBold : 0);
            run.clear();
        };
        for (size_t i = 0; i < shown.size(); ++cp) {
            size_t start = i;
            text::decode_utf8(shown, i);
            bool hit = cp < marks.size() && marks[cp];
            if (hit != run_hit) { flush(); run_hit = hit; }
            run.append(shown, start, i - start);
        }
        flush();
        line += std::string(static_cast<size_t>(std::max(1, name_w - text::display_width(shown) + 2)), ' ');
        line += out.c(text::truncate_path(pretty_path(st.parent_path(id)), width - name_w - 12, fileio::kPathSep), theme::kFaint);
        out.line(line);
    }
    out.line();
    std::string summary = fmt::thousands(r.size()) + (r.size() == 1 ? " result" : " results") + " · " + fmt::query_time(r.ms);
    if (r.size() > n) summary += " · showing " + fmt::thousands(n) + "  (run " + "pearch " + r.query.raw + " to browse all)";
    out.line("  " + out.c(summary, theme::kFaint));
    out.line();
    return 0;
}

int cmd_search(const Options& o) {
    App app(o);
    const bool interactive = interactive_terminal() && !o.list;
    term::Terminal t;
    if (interactive) t.start(true);  // falls back to a list below if this fails
    std::string err;
    std::shared_ptr<engine::IndexStore> store = app.load_store(&err);
    bool fresh = false;

    if (!store) {
        // First run: build the index right away, with the pear.
        auto r = app.build(app.config.scan_options(), 0, "Setting up — indexing your files", interactive, o.quiet,
                           "opening search…", 0.9, &t);
        if (!r.store) {
            t.stop();
            eprint(app.mode, "Indexing cancelled. Run pearch again whenever you're ready.");
            return 1;
        }
        store = r.store;
        app.save_store(*store);
        fresh = true;
    }

    uint32_t scope = engine::kNone;
    std::string scope_label;
    bool temporary = false;
    if (o.in_dir) {
        const std::string abs = fileio::absolute(*o.in_dir);
        if (!fileio::is_directory(abs)) {
            t.stop();
            eprint(app.mode, "Not a folder: " + abs);
            return 1;
        }
        scope_label = pretty_path(abs);
        uint32_t id = store->find_path(abs);
        if (id != engine::kNone && store->is_dir(id)) {
            scope = id;
        } else {
            // Outside the indexed locations: index just this folder for now.
            scan::Options opt = app.config.scan_options();
            opt.roots = {abs};
            auto r = app.build(opt, 0, "Indexing " + scope_label, interactive, o.quiet, "opening search…", 0.5, &t);
            if (!r.store) { t.stop(); return 1; }
            store = r.store;
            scope = 0;
            temporary = true;
        }
    }

    engine::History history;
    history.load(app.paths.history);

    if (!interactive) {
        t.stop();
        engine::ThreadPool pool;
        engine::Searcher searcher(pool);
        engine::SearchRequest req;
        req.text = o.query();
        req.scope = scope;
        req.now_ms = fmt::now_ms();
        req.boosts = history.resolve(*store, req.now_ms);
        auto r = searcher.search(store, req);
        return print_results(app, r, o);
    }

    if (!t.active() && !t.start(true)) {
        eprint(app.mode, "This terminal doesn't support the full-screen UI; showing a list instead.");
        Options list = o;
        list.list = true;
        return cmd_search(list);
    }
    tui::SearchSession s;
    s.store = store;
    s.query = o.query();
    s.scope = scope;
    s.scope_label = scope_label;
    s.history = &history;
    s.home = shell::home_dir();
    const std::string history_file = app.paths.history;
    s.save_history = [&history, history_file] { history.save(history_file); };
    if (!temporary) {
        // Background refresh: half the scan threads at low priority, so typing stays instant.
        scan::Options opt = app.config.scan_options();
        opt.threads = std::max(2u, hw::scan_plan(opt.roots).threads / 2);
        opt.background = true;
        s.rebuild = [opt](scan::Progress& p) { return scan::build(opt, p, fmt::now_ms()); };
        const std::string index_file = app.paths.index;
        s.on_rebuilt = [index_file](const engine::IndexStore& st) { st.save(index_file); };
        const int64_t age_ms = fmt::now_ms() - store->built_at();
        s.refresh_now = !fresh && app.config.refresh_minutes > 0 &&
                        age_ms > static_cast<int64_t>(app.config.refresh_minutes) * 60000;
    }
    int rc = tui::run_search_view(t, app.mode, s);
    t.stop();
    return rc;
}

}  // namespace

int run(const Options& o) {
    if (!o.error.empty()) {
        term::prepare_output();
        eprint(term::stderr_is_tty() ? term::detect_color(o.no_color) : term::ColorMode::None, o.error);
        return 2;
    }
    switch (o.cmd) {
        case Command::Help: return cmd_help(o);
        case Command::Version: return cmd_version(o);
        case Command::Index: return cmd_index(o);
        case Command::Add: return cmd_add(o);
        case Command::Remove: return cmd_remove(o);
        case Command::Locations: return cmd_locations(o);
        case Command::Stats: return cmd_stats(o);
        case Command::Install: return cmd_install(o, true);
        case Command::Uninstall: return cmd_install(o, false);
        case Command::Scan: return cmd_scan(o);
        case Command::PearDemo: return cmd_pear(o);
        case Command::Tour: return cmd_tour(o);
        case Command::Search: return cmd_search(o);
    }
    return 0;
}

}  // namespace app
