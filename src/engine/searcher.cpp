#include "engine/searcher.h"

#include <algorithm>
#include <chrono>
#include <unordered_map>

#include "core/text.h"
#include "engine/matcher.h"

namespace engine {

const char* sort_mode_name(SortMode m) noexcept {
    switch (m) {
        case SortMode::Relevance: return "relevance";
        case SortMode::Name: return "name";
        case SortMode::Modified: return "date modified";
        case SortMode::Size: return "size";
    }
    return "";
}

namespace {

constexpr size_t kParallelMin = 32768;  // below this a single thread is faster
constexpr size_t kFuzzyThreshold = 25;  // fall back to fuzzy below this many hits
constexpr size_t kInitialSorted = 256;
constexpr size_t kRecentLimit = 300;
constexpr int64_t kDay = 86400000LL;

using Clock = std::chrono::steady_clock;

// Runs fn(lo, hi, out) over chunks of [lo, hi) on the pool and concatenates
// the per-chunk outputs in order (so ascending ids stay ascending).
template <typename T, typename Fn>
std::vector<T> parallel_collect(ThreadPool& pool, size_t lo, size_t hi, size_t min_chunk, Fn&& fn) {
    const size_t n = hi > lo ? hi - lo : 0;
    std::vector<T> out;
    if (n == 0) return out;
    if (n < min_chunk * 2 || pool.size() == 1) {
        fn(lo, hi, out);
        return out;
    }
    const size_t chunks = std::min<size_t>(pool.size() * 4, n / min_chunk);
    std::vector<std::vector<T>> parts(chunks);
    pool.parallel_for(chunks, [&](size_t c) {
        fn(lo + n * c / chunks, lo + n * (c + 1) / chunks, parts[c]);
    });
    size_t total = 0;
    for (const auto& p : parts) total += p.size();
    out.reserve(total);
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

struct Ctx {
    const IndexStore& s;
    const Query& q;
    const SearchRequest& req;
    uint32_t lo = 0, hi = 0;
    std::vector<const Term*> pos_substr;
    const Term* pos_wild = nullptr;
    const Term* pos_in = nullptr;
    const Term* pos_path = nullptr;
    size_t max_term_len = 0;

    Ctx(const IndexStore& store, const Query& query, const SearchRequest& r)
        : s(store), q(query), req(r) {
        for (const Term& t : q.terms) {
            if (t.negate) continue;
            switch (t.kind) {
                case Term::Substr:
                    pos_substr.push_back(&t);
                    max_term_len = std::max(max_term_len, t.text.size());
                    break;
                case Term::Wildcard: if (!pos_wild) pos_wild = &t; break;
                case Term::InFolder: if (!pos_in) pos_in = &t; break;
                case Term::Path: if (!pos_path) pos_path = &t; break;
            }
        }
    }

    bool ancestor_contains(uint32_t i, std::string_view t) const {
        for (uint32_t p = s.parent(i); p != kNone; p = s.parent(p))
            if (match::contains(s.folded(p), t)) return true;
        return false;
    }

    std::string folded_path(uint32_t i) const {
        uint32_t chain[256];
        size_t n = 0;
        for (uint32_t p = i; p != kNone && n < 256; p = s.parent(p)) chain[n++] = p;
        std::string out = text::fold(s.stored_name(chain[n - 1]));
        for (size_t k = n - 1; k-- > 0;) {
            if (!out.empty() && out.back() != fileio::kPathSep) out += fileio::kPathSep;
            out += s.folded(chain[k]);
        }
        return out;
    }

    bool filters_ok(uint32_t i) const {
        const uint8_t f = s.flags(i);
        switch (q.type) {
            case TypeFilter::All: break;
            case TypeFilter::Files: if (f & kDir) return false; break;
            case TypeFilter::Folders: if (!(f & kDir)) return false; break;
            case TypeFilter::Apps: if (!(f & kApp)) return false; break;
        }
        if (!q.exts.empty() || !q.ext_prefixes.empty()) {
            if (f & kDir) return false;
            std::string_view ext = text::extension_of(s.folded(i));
            if (ext.empty()) return false;
            bool hit = false;
            for (const auto& e : q.exts)
                if (ext == e) { hit = true; break; }
            for (const auto& e : q.ext_prefixes)
                if (ext.starts_with(e)) { hit = true; break; }
            if (!hit) return false;
        }
        if (q.size_min || q.size_max) {
            if (f & kDir) return false;
            uint64_t sz = s.file_size(i);
            if (q.size_min && sz < *q.size_min) return false;
            if (q.size_max && sz > *q.size_max) return false;
        }
        if (q.modified_after && s.mtime(i) < *q.modified_after) return false;
        return true;
    }

    // Full check of one entry. In fuzzy mode positive words only need their
    // letters to appear in order in the name.
    // `known` is a term the caller already found in the name (from the scan).
    template <bool Fuzzy>
    bool verify(uint32_t i, const Term* known = nullptr) const {
        if (!filters_ok(i)) return false;
        std::string_view name = s.folded(i);
        bool name_hit = false;
        std::string path;  // built lazily for path terms
        for (const Term& t : q.terms) {
            switch (t.kind) {
                case Term::Substr: {
                    bool in_name = &t == known || match::contains(name, t.text);
                    if (t.negate) {
                        if (in_name) return false;
                    } else if (in_name) {
                        name_hit = true;
                    } else if ((s.flags(i) & kApp) && match::initials(name, s.name(i), t.text)) {
                        name_hit = true;  // "vs code" -> Visual Studio Code
                    } else if constexpr (Fuzzy) {
                        // Short words must still match exactly (name or folder).
                        if (t.text.size() < 3) {
                            if (!ancestor_contains(i, t.text)) return false;
                        } else {
                            if (match::fuzzy(name, t.text) < 0) return false;
                            name_hit = true;
                        }
                    } else if (!ancestor_contains(i, t.text)) {
                        return false;
                    }
                    break;
                }
                case Term::Wildcard: {
                    bool m = match::glob(name, t.text);
                    if (m == t.negate) return false;
                    if (m) name_hit = true;
                    break;
                }
                case Term::InFolder:
                    if (ancestor_contains(i, t.text) == t.negate) return false;
                    break;
                case Term::Path:
                    if (path.empty()) path = folded_path(i);
                    if (match::contains(path, t.text) == t.negate) return false;
                    break;
            }
        }
        // At least one word has to be in the name itself, otherwise "documents"
        // would return everything inside Documents.
        if ((!pos_substr.empty() || pos_wild) && !name_hit) return false;
        return true;
    }

    int32_t score(uint32_t i, bool fuzzy) const {
        std::string_view name = s.folded(i);
        const uint8_t f = s.flags(i);
        int32_t base;
        int32_t best = 0;
        if (!pos_substr.empty()) {
            int32_t sum = 0;
            std::string_view original = s.name(i);
            for (const Term* t : pos_substr) {
                int32_t tier;
                if (t->text.size() == 1) {  // one letter: only exact/prefix matter
                    const char c = t->text[0];
                    tier = name.size() == 1 && name[0] == c ? 1000
                           : !name.empty() && name[0] == c  ? 800
                           : name.find(c) != std::string_view::npos ? 500 : 0;
                } else {
                    tier = match::name_tier(name, original, t->text).score;
                    if (tier == 0 && (f & kApp) && match::initials(name, original, t->text)) tier = 700;
                }
                if (tier == 0) {
                    int fz = fuzzy ? match::fuzzy(name, t->text) : -1;
                    tier = fz >= 0 ? 200 + fz / 2 : 60;  // fuzzy hit or parent-folder hit
                }
                best = std::max(best, tier);
                sum += tier;
            }
            base = best + (sum - best) / 4;
        } else if (pos_wild) {
            base = 700;
        } else {
            base = 500;
        }
        const size_t len = name.size();
        base -= static_cast<int32_t>(std::min<size_t>(60, len - std::min(len, max_term_len)));
        base -= std::min<int32_t>(80, s.depth(i) * 4);
        const int64_t age = req.now_ms - s.mtime(i);
        if (age < kDay) base += 60;
        else if (age < 7 * kDay) base += 40;
        else if (age < 30 * kDay) base += 20;
        else if (age < 365 * kDay) base += 5;
        if (f & (kHidden | kSystem)) base -= 120;
        if (!name.empty() && name[0] == '.') base -= 40;
        if (f & kApp) base += best >= 650 ? 400 : 120;
        if (req.boosts) base += req.boosts->get(i);
        return base;
    }
};

std::vector<uint32_t> scan_term(ThreadPool& pool, const IndexStore& s, uint32_t lo, uint32_t hi,
                                std::string_view term) {
    const char* blob = s.folded_blob();
    const uint32_t* off = s.fold_offsets();
    return parallel_collect<uint32_t>(pool, lo, hi, kParallelMin / 2,
                                      [&](size_t a, size_t b, std::vector<uint32_t>& out) {
        if (term.size() == 1) {
            // One letter matches most names: test entry by entry.
            const char c = term[0];
            out.reserve((b - a) / 2);
            for (size_t i = a; i < b; ++i) {
                const char* p = blob + off[i];
                const char* e = blob + off[i + 1] - 1;
                for (; p < e; ++p)
                    if (*p == c) { out.push_back(static_cast<uint32_t>(i)); break; }
            }
            return;
        }
        size_t pos = off[a];
        const size_t end = off[b];
        size_t i = a;
        while (pos < end) {
            size_t p = match::find(blob + pos, end - pos, term.data(), term.size());
            if (p == match::npos) break;
            const size_t at = pos + p;
            while (off[i + 1] <= at) ++i;
            out.push_back(static_cast<uint32_t>(i));
            pos = off[i + 1];  // one hit per entry is enough
        }
    });
}

// Housekeeping files nobody means by "recent".
bool is_noise(std::string_view name) {
    if (name.empty() || name[0] == '.' || name.starts_with("~$")) return true;
    std::string_view ext = text::extension_of(name);
    static const char* const kNoise[] = {"pyc", "pyo", "tmp", "temp", "log", "lock", "etl", "cache", "crdownload",
                                         "part", "partial", "swp", "db-journal", "db-wal", "db-shm", "ini", "dat",
                                         "blf", "regtrans-ms", "pf", "dmp", "o", "obj", "pdb", "tlog", "idb"};
    for (const char* n : kNoise)
        if (ext == n) return true;
    return name == "thumbs.db" || name == "desktop.ini" || name == "ntuser.dat";
}

bool in_hidden_folder(const IndexStore& s, uint32_t id) {
    for (uint32_t p = s.parent(id); p != kNone && !s.is_root(p); p = s.parent(p)) {
        if (s.flags(p) & (kHidden | kSystem)) return true;
        std::string_view n = s.folded(p);
        if (n.starts_with('.') || n == "__pycache__" || n == "cache" || n == "temp" || n == "tmp") return true;
    }
    return false;
}

// The same shortcut on the Desktop and in the Start Menu is one app: keep the best.
void dedupe_apps(const IndexStore& s, std::vector<Hit>& hits) {
    std::unordered_map<std::string_view, size_t> seen;
    bool any = false;
    for (size_t k = 0; k < hits.size(); ++k) {
        if (!(s.flags(hits[k].id) & kApp)) continue;
        auto [it, inserted] = seen.emplace(s.folded(hits[k].id), k);
        if (inserted) continue;
        size_t& keep = it->second;
        size_t loser = k;
        if (hits[k].score > hits[keep].score) { loser = keep; keep = k; }
        hits[loser].id = kNone;
        any = true;
    }
    if (any) hits.erase(std::remove_if(hits.begin(), hits.end(), [](const Hit& h) { return h.id == kNone; }), hits.end());
}

// Ranges covered by the given folders (ascending ids): their subtrees,
// optionally including the folder itself.
std::vector<uint32_t> subtree_ids(const IndexStore& s, const std::vector<uint32_t>& ids,
                                  bool include_self, bool dirs_only_source, uint32_t lo, uint32_t hi) {
    std::vector<uint32_t> out;
    uint32_t covered = 0;
    for (uint32_t d : ids) {
        if (dirs_only_source && !s.is_dir(d)) continue;
        uint32_t a = include_self ? d : d + 1;
        uint32_t b = s.is_dir(d) ? s.subtree_end(d) : d + 1;
        a = std::max({a, covered, lo});
        b = std::min(b, hi);
        for (uint32_t k = a; k < b; ++k) out.push_back(k);
        covered = std::max(covered, b);
    }
    return out;
}

}  // namespace

bool SearchResult::before(const Hit& a, const Hit& b) const noexcept {
    const IndexStore& s = *store;
    switch (sort) {
        case SortMode::Relevance:
            if (a.score != b.score) return a.score > b.score;
            break;
        case SortMode::Name: {
            auto na = s.folded(a.id), nb = s.folded(b.id);
            if (na != nb) return na < nb;
            break;
        }
        case SortMode::Modified:
            if (s.mtime(a.id) != s.mtime(b.id)) return s.mtime(a.id) > s.mtime(b.id);
            break;
        case SortMode::Size:
            if (s.file_size(a.id) != s.file_size(b.id)) return s.file_size(a.id) > s.file_size(b.id);
            break;
    }
    if (a.score != b.score) return a.score > b.score;
    return a.id < b.id;  // ties: index order (keeps folders together, and is cheap)
}

void SearchResult::ensure_sorted(size_t n) {
    n = std::min(n, hits_.size());
    if (n <= sorted_) return;
    size_t target = std::min(hits_.size(), std::max({n, sorted_ * 2, kInitialSorted}));
    auto first = hits_.begin() + static_cast<std::ptrdiff_t>(sorted_);
    auto mid = hits_.begin() + static_cast<std::ptrdiff_t>(target);
    // Select the next window in O(n), then sort only the window.
    auto select = [&](auto cmp) {
        if (mid != hits_.end()) std::nth_element(first, mid, hits_.end(), cmp);
        std::sort(first, mid, cmp);
    };
    if (sort == SortMode::Relevance) {
        select([](const Hit& a, const Hit& b) { return a.score != b.score ? a.score > b.score : a.id < b.id; });
    } else {
        select([this](const Hit& a, const Hit& b) { return before(a, b); });
    }
    sorted_ = target;
}

void SearchResult::select_top(ThreadPool& pool, size_t k) {
    const size_t n = hits_.size();
    k = std::min(k, n);
    if (k == 0) return;
    auto run = [&](auto cmp) {
        // Every chunk finds its own best k in parallel; the overall k-th best
        // is among those, and one partition pass moves the winners up front.
        const size_t chunks = std::max<size_t>(1, std::min<size_t>(pool.size() * 2, n / 8192));
        std::vector<std::vector<Hit>> fronts(chunks);
        pool.parallel_for(chunks, [&](size_t c) {
            auto b = hits_.begin() + static_cast<std::ptrdiff_t>(n * c / chunks);
            auto e = hits_.begin() + static_cast<std::ptrdiff_t>(n * (c + 1) / chunks);
            const auto take = std::min<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(k), e - b);
            if (take < e - b) std::nth_element(b, b + take, e, cmp);
            fronts[c].assign(b, b + take);
        });
        std::vector<Hit> best;
        for (auto& f : fronts) best.insert(best.end(), f.begin(), f.end());
        std::nth_element(best.begin(), best.begin() + static_cast<std::ptrdiff_t>(k - 1), best.end(), cmp);
        const Hit kth = best[k - 1];
        auto mid = std::partition(hits_.begin(), hits_.end(), [&](const Hit& h) { return !cmp(kth, h); });
        std::sort(hits_.begin(), mid, cmp);
        sorted_ = static_cast<size_t>(mid - hits_.begin());
    };
    if (sort == SortMode::Relevance) run([](const Hit& a, const Hit& b) { return a.score != b.score ? a.score > b.score : a.id < b.id; });
    else run([this](const Hit& a, const Hit& b) { return before(a, b); });
}

uint32_t SearchResult::id(size_t k) {
    ensure_sorted(k + 1);
    return hits_[k].id;
}

int32_t SearchResult::score(size_t k) {
    ensure_sorted(k + 1);
    return hits_[k].score;
}

const std::vector<uint32_t>& Searcher::app_ids(const IndexStore& s) {
    if (apps_for_ != &s) {
        apps_ = parallel_collect<uint32_t>(pool_, 0, s.size(), kParallelMin, [&](size_t a, size_t b, std::vector<uint32_t>& out) {
            for (size_t i = a; i < b; ++i)
                if (s.flags(static_cast<uint32_t>(i)) & kApp) out.push_back(static_cast<uint32_t>(i));
        });
        apps_for_ = &s;
    }
    return apps_;
}

SearchResult Searcher::search(std::shared_ptr<const IndexStore> store, const SearchRequest& req) {
    const auto t0 = Clock::now();
    SearchResult r;
    r.store = store;
    r.sort = req.sort;
    r.query = parse_query(req.text, req.now_ms);
    if (req.tab != TypeFilter::All) r.query.type = req.tab;
    // Only filters (*.pdf, modified:today, in:work): newest first reads best.
    if (req.sort == SortMode::Relevance && !r.query.terms.empty() && !r.query.has_positive_name_terms())
        r.sort = SortMode::Modified;
    if (req.sort == SortMode::Relevance && r.query.terms.empty() && r.query.has_filters())
        r.sort = SortMode::Modified;
    if (!store || store->empty()) return r;

    const IndexStore& s = *store;
    Ctx ctx(s, r.query, req);
    ctx.lo = 0;
    ctx.hi = static_cast<uint32_t>(s.size());
    if (req.scope != kNone && req.scope < s.size()) {
        ctx.lo = req.scope + 1;
        ctx.hi = s.subtree_end(req.scope);
    }
    const Query& q = r.query;

    auto verify_and_score = [&](const std::vector<uint32_t>& cand, std::vector<uint32_t>* strict_out,
                                const Term* known) {
        auto hits = parallel_collect<Hit>(pool_, 0, cand.size(), kParallelMin / 4,
                                          [&](size_t a, size_t b, std::vector<Hit>& out) {
            out.reserve(b - a);
            for (size_t k = a; k < b; ++k) {
                uint32_t i = cand[k];
                if (ctx.verify<false>(i, known)) out.push_back({ctx.score(i, false), i});
            }
        });
        if (strict_out) {
            strict_out->clear();
            strict_out->reserve(hits.size());
            for (const Hit& h : hits) strict_out->push_back(h.id);
        }
        return hits;
    };

    const bool empty_text = text::trim(req.text).empty();
    if (empty_text || (q.terms.empty() && !q.has_filters())) {
        // Recent view: things you opened, then recently modified files.
        r.recent = true;
        last_valid_ = false;
        if (q.type == TypeFilter::Apps) {
            r.sort = SortMode::Name;
            for (uint32_t id : app_ids(s))
                if (id >= ctx.lo && id < ctx.hi) r.hits_.push_back({0, id});
            dedupe_apps(s, r.hits_);
        } else {
            std::vector<uint32_t> picked;
            if (req.boosts) {
                for (uint32_t id : req.boosts->ranked) {
                    if (id < ctx.lo || id >= ctx.hi || !ctx.filters_ok(id)) continue;
                    picked.push_back(id);
                }
            }
            const bool want_dirs = q.type == TypeFilter::Folders;
            auto recent = parallel_collect<Hit>(pool_, ctx.lo, ctx.hi, kParallelMin,
                                                [&](size_t a, size_t b, std::vector<Hit>& out) {
                for (size_t i = a; i < b; ++i) {
                    auto id = static_cast<uint32_t>(i);
                    uint8_t f = s.flags(id);
                    if ((f & (kHidden | kSystem | kRoot)) || bool(f & kDir) != want_dirs) continue;
                    if (s.mtime(id) > req.now_ms + kDay || is_noise(s.folded(id))) continue;
                    out.push_back({0, id});
                }
                auto by_time = [&](const Hit& x, const Hit& y) { return s.mtime(x.id) > s.mtime(y.id); };
                if (out.size() > kRecentLimit) {
                    std::partial_sort(out.begin(), out.begin() + kRecentLimit, out.end(), by_time);
                    out.resize(kRecentLimit);
                }
            });
            std::sort(recent.begin(), recent.end(),
                      [&](const Hit& x, const Hit& y) { return s.mtime(x.id) > s.mtime(y.id); });
            for (const Hit& h : recent) {
                if (picked.size() >= kRecentLimit) break;
                if (req.boosts && req.boosts->by_id.count(h.id)) continue;
                if (in_hidden_folder(s, h.id)) continue;  // caches, .git, __pycache__ ...
                picked.push_back(h.id);
            }
            int32_t score = 1'000'000;
            for (uint32_t id : picked) r.hits_.push_back({score--, id});
        }
    } else {
        std::vector<uint32_t> strict;
        const bool can_refine = last_valid_ && last_store_ == store && last_scope_ == req.scope &&
                                q.refines(last_query_);
        if (can_refine) {
            r.hits_ = verify_and_score(last_strict_, &strict, nullptr);
        } else {
            std::vector<uint32_t> cand;
            const Term* known = nullptr;  // in every candidate's name already
            bool all = false;
            if (!ctx.pos_substr.empty()) {
                if (ctx.pos_substr.size() == 1) known = ctx.pos_substr[0];
                cand = scan_term(pool_, s, ctx.lo, ctx.hi, ctx.pos_substr[0]->text);
                auto add = [&](const std::vector<uint32_t>& more) {
                    std::vector<uint32_t> merged;
                    merged.reserve(cand.size() + more.size());
                    std::set_union(cand.begin(), cand.end(), more.begin(), more.end(), std::back_inserter(merged));
                    cand.swap(merged);
                };
                for (size_t k = 1; k < ctx.pos_substr.size(); ++k)
                    add(scan_term(pool_, s, ctx.lo, ctx.hi, ctx.pos_substr[k]->text));
                // Apps can also match by their initials, which no substring scan finds.
                const auto& apps = app_ids(s);
                std::vector<uint32_t> in_range;
                for (uint32_t id : apps)
                    if (id >= ctx.lo && id < ctx.hi) in_range.push_back(id);
                if (!in_range.empty()) {
                    known = nullptr;
                    add(in_range);
                }
            } else if (ctx.pos_wild) {
                std::string_view lit = match::longest_literal(ctx.pos_wild->text);
                if (lit.empty()) all = true;
                else cand = scan_term(pool_, s, ctx.lo, ctx.hi, lit);
            } else if (ctx.pos_in) {
                auto dirs = scan_term(pool_, s, ctx.lo, ctx.hi, ctx.pos_in->text);
                cand = subtree_ids(s, dirs, false, true, ctx.lo, ctx.hi);
            } else if (ctx.pos_path) {
                // The longest path segment must occur in the entry's own name
                // or in one of its folders: use it to pick candidate subtrees.
                std::string_view term = ctx.pos_path->text, seg;
                for (size_t a = 0; a < term.size();) {
                    size_t b = term.find(fileio::kPathSep, a);
                    if (b == std::string_view::npos) b = term.size();
                    if (b - a > seg.size()) seg = term.substr(a, b - a);
                    a = b + 1;
                }
                if (seg.empty()) all = true;
                else cand = subtree_ids(s, scan_term(pool_, s, ctx.lo, ctx.hi, seg), true, false, ctx.lo, ctx.hi);
            } else {
                all = true;  // only filters / exclusions
            }
            if (all) {
                r.hits_ = parallel_collect<Hit>(pool_, ctx.lo, ctx.hi, kParallelMin,
                                                [&](size_t a, size_t b, std::vector<Hit>& out) {
                    for (size_t i = a; i < b; ++i) {
                        auto id = static_cast<uint32_t>(i);
                        if (s.is_root(id)) continue;  // the locations themselves aren't results here
                        if (ctx.verify<false>(id)) out.push_back({ctx.score(id, false), id});
                    }
                });
                for (const Hit& h : r.hits_) strict.push_back(h.id);
            } else {
                r.hits_ = verify_and_score(cand, &strict, known);
            }
        }

        // Few exact hits: also show names that contain the letters in order.
        bool fuzzy_ok = false;
        for (const Term* t : ctx.pos_substr) fuzzy_ok = fuzzy_ok || t->text.size() >= 3;
        if (r.hits_.size() < kFuzzyThreshold && fuzzy_ok) {
            std::string all_letters;
            for (const Term* t : ctx.pos_substr)
                if (t->text.size() >= 3) all_letters += t->text;
            const uint64_t need = text::char_mask(all_letters);
            auto fuzzy = parallel_collect<Hit>(pool_, ctx.lo, ctx.hi, kParallelMin,
                                               [&](size_t a, size_t b, std::vector<Hit>& out) {
                size_t si = std::lower_bound(strict.begin(), strict.end(), static_cast<uint32_t>(a)) - strict.begin();
                for (size_t i = a; i < b; ++i) {
                    auto id = static_cast<uint32_t>(i);
                    if ((s.mask(id) & need) != need) continue;
                    while (si < strict.size() && strict[si] < id) ++si;
                    if (si < strict.size() && strict[si] == id) continue;
                    if (ctx.verify<true>(id)) out.push_back({ctx.score(id, true), id});
                }
            });
            if (!fuzzy.empty()) {
                r.fuzzy = true;
                r.hits_.insert(r.hits_.end(), fuzzy.begin(), fuzzy.end());
            }
        }

        dedupe_apps(s, r.hits_);
        last_store_ = store;
        last_query_ = q;
        last_scope_ = req.scope;
        last_strict_ = std::move(strict);
        last_valid_ = true;
    }

    if (r.hits_.size() <= 4096) r.ensure_sorted(r.hits_.size());
    else if (r.hits_.size() >= 65536 && pool_.size() > 1) r.select_top(pool_, kInitialSorted);
    else r.ensure_sorted(kInitialSorted);
    r.ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return r;
}

AsyncSearcher::AsyncSearcher(ThreadPool& pool, std::function<void()> on_done)
    : searcher_(pool), on_done_(std::move(on_done)), thread_([this] { loop(); }) {}

AsyncSearcher::~AsyncSearcher() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

uint64_t AsyncSearcher::submit(std::shared_ptr<const IndexStore> store, SearchRequest req) {
    std::lock_guard<std::mutex> lk(m_);
    pending_store_ = std::move(store);
    pending_ = std::move(req);
    busy_ = true;
    cv_.notify_one();
    return ++generation_;
}

std::unique_ptr<SearchResult> AsyncSearcher::take() {
    std::lock_guard<std::mutex> lk(m_);
    return std::move(done_);
}

void AsyncSearcher::loop() {
    std::unique_lock<std::mutex> lk(m_);
    for (;;) {
        cv_.wait(lk, [&] { return stop_ || pending_.has_value(); });
        if (stop_) return;
        SearchRequest req = std::move(*pending_);
        pending_.reset();
        auto store = std::move(pending_store_);
        const uint64_t gen = generation_;
        lk.unlock();
        auto result = std::make_unique<SearchResult>(searcher_.search(store, req));
        result->generation = gen;
        lk.lock();
        done_ = std::move(result);
        if (!pending_) busy_ = false;
        if (on_done_) on_done_();
    }
}

}  // namespace engine
