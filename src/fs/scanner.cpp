#include "fs/scanner.h"

#include <algorithm>
#include <condition_variable>
#include <thread>
#include <unordered_set>

#include "core/fileio.h"
#include "core/hardware.h"
#include "core/text.h"

namespace scan {

void Progress::set_current(const std::string& path) {
    std::unique_lock<std::mutex> lk(m_, std::try_to_lock);
    if (lk.owns_lock()) current_ = path;
}

std::string Progress::current() const {
    std::lock_guard<std::mutex> lk(m_);
    return current_;
}

namespace {

using engine::kNone;

struct Rec {
    uint32_t name_off, name_len;
    uint32_t fold_off, fold_len;
    uint32_t child_dir;  // dir id to descend into, or kNone
    uint8_t flags;
    uint64_t size;
    int64_t mtime;
};

struct Arena {
    std::string names;
    std::string folded;
    std::vector<Rec> recs;
};

struct DirInfo {
    uint32_t worker = 0;
    uint32_t first = 0;
    uint32_t count = 0;
};

struct Task {
    uint32_t dir_id;
    detail::NativePath native;
    std::string utf8;
    bool app;
};

bool is_app_file(std::string_view folded_name, bool in_app_folder) {
    std::string_view ext = text::extension_of(folded_name);
    if (ext == "lnk" || ext == "url" || ext == "appref-ms") return true;
    return in_app_folder && ext == "exe";
}

class Scanner {
public:
    Scanner(const Options& opt, Progress& progress) : opt_(opt), progress_(progress) {
        for (const auto& n : opt.skip_names) skip_names_.insert(text::fold(n));
        for (const auto& p : opt.skip_paths) stop_paths_.insert(text::fold(fileio::absolute(p)));
        for (const auto& p : opt.app_paths) app_paths_.insert(text::fold(fileio::absolute(p)));
        // Another root inside this one is indexed as its own location.
        for (const auto& r : opt.roots) stop_paths_.insert(text::fold(r));
    }

    std::shared_ptr<engine::IndexStore> run(int64_t now_ms) {
        // Roots that exist, in order, without duplicates.
        std::vector<std::pair<std::string, int64_t>> roots;
        std::unordered_set<std::string> seen;
        for (const auto& r : opt_.roots) {
            int64_t mtime = 0;
            std::string abs = fileio::absolute(r);
            if (!seen.insert(text::fold(abs)).second) continue;
            if (!detail::stat_dir(detail::to_native(abs), mtime)) {
                progress_.errors.fetch_add(1);
                continue;
            }
            roots.emplace_back(abs, mtime);
        }

        unsigned threads = opt_.threads;
        if (threads == 0) {
            std::vector<std::string> existing;
            for (const auto& r : roots) existing.push_back(r.first);
            threads = hw::scan_plan(existing).threads;
        }
        arenas_.resize(threads);
        for (uint32_t k = 0; k < roots.size(); ++k) {
            stack_.push_back({k, detail::to_native(roots[k].first), roots[k].first,
                              app_paths_.count(text::fold(roots[k].first)) > 0});
        }
        next_dir_ = static_cast<uint32_t>(roots.size());
        infos_.resize(roots.size());
        progress_.dirs_queued.fetch_add(roots.size());

        std::vector<std::thread> pool;
        for (unsigned w = 0; w < threads; ++w) pool.emplace_back([this, w] { worker(w); });
        for (auto& t : pool) t.join();
        if (progress_.cancel.load()) return nullptr;

        // Stitch the arenas together in depth-first order.
        size_t total = roots.size(), bytes = 0;
        for (const auto& a : arenas_) { total += a.recs.size(); bytes += a.names.size(); }
        engine::IndexBuilder b;
        b.reserve(total, bytes);
        struct Frame { uint32_t dir, parent, pos; };
        std::vector<Frame> st;
        for (uint32_t k = 0; k < roots.size(); ++k) {
            uint32_t root_id = b.add_root(roots[k].first, roots[k].second);
            st.push_back({k, root_id, 0});
            while (!st.empty()) {
                Frame& fr = st.back();
                const DirInfo& info = infos_[fr.dir];
                if (fr.pos >= info.count) { st.pop_back(); continue; }
                const Arena& a = arenas_[info.worker];
                const Rec& r = a.recs[info.first + fr.pos++];
                uint32_t parent = fr.parent;  // fr may dangle after push_back
                uint32_t id = b.add_raw(parent, std::string_view(a.names).substr(r.name_off, r.name_len),
                                        std::string_view(a.folded).substr(r.fold_off, r.fold_len),
                                        r.flags, r.size, r.mtime);
                if (r.child_dir != kNone && id != kNone) st.push_back({r.child_dir, id, 0});
            }
        }
        arenas_.clear();
        auto store = b.finish(now_ms);
        progress_.finished = true;
        return store;
    }

private:
    void worker(unsigned w) {
        if (opt_.background) detail::lower_thread_priority();
        Arena& arena = arenas_[w];
        std::vector<detail::DirEntry> entries;
        std::vector<Task> found;
        unsigned tick = 0;
        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] { return !stack_.empty() || active_ == 0 || progress_.cancel.load(); });
                if (progress_.cancel.load() || (stack_.empty() && active_ == 0)) {
                    cv_.notify_all();
                    return;
                }
                task = std::move(stack_.back());
                stack_.pop_back();
                ++active_;
            }
            if ((tick++ & 7) == 0) progress_.set_current(task.utf8);
            found.clear();
            process(w, arena, task, entries, found);
            {
                std::lock_guard<std::mutex> lk(m_);
                for (auto& t : found) stack_.push_back(std::move(t));
                --active_;
            }
            if (found.size() > 1) cv_.notify_all();
            else cv_.notify_one();
        }
    }

    void process(unsigned w, Arena& arena, const Task& task, std::vector<detail::DirEntry>& entries,
                 std::vector<Task>& found) {
        entries.clear();
        DirInfo info;
        info.worker = w;
        info.first = static_cast<uint32_t>(arena.recs.size());
        if (!detail::list_dir(task.native, entries)) {
            progress_.errors.fetch_add(1);
        }
        uint64_t files = 0, dirs = 0;
        for (auto& e : entries) {
            Rec r{};
            r.name_off = static_cast<uint32_t>(arena.names.size());
            r.name_len = static_cast<uint32_t>(e.name.size());
            arena.names += e.name;
            r.fold_off = static_cast<uint32_t>(arena.folded.size());
            arena.folded += text::fold(e.name);
            r.fold_len = static_cast<uint32_t>(arena.folded.size() - r.fold_off);
            std::string_view folded(arena.folded.data() + r.fold_off, r.fold_len);
            r.flags = (e.is_dir ? engine::kDir : 0) | (e.hidden ? engine::kHidden : 0) |
                      (e.system ? engine::kSystem : 0) | (e.link ? engine::kLink : 0);
            r.size = e.size;
            r.mtime = e.mtime;
            r.child_dir = kNone;
            if (e.is_dir) {
                ++dirs;
                if (!e.link && !skip_names_.count(std::string(folded))) {
                    std::string child = fileio::join(task.utf8, e.name);
                    std::string child_folded = text::fold(child);
                    if (!stop_paths_.count(child_folded)) {
                        r.child_dir = next_dir_.fetch_add(1);
                        found.push_back({r.child_dir, detail::join(task.native, e.name), std::move(child),
                                         task.app || app_paths_.count(child_folded) > 0});
                    }
                }
            } else {
                ++files;
                if (is_app_file(folded, task.app)) r.flags |= engine::kApp;
            }
            arena.recs.push_back(r);
        }
        info.count = static_cast<uint32_t>(arena.recs.size() - info.first);
        {
            std::lock_guard<std::mutex> lk(info_m_);
            uint32_t needed = next_dir_.load();
            if (infos_.size() < needed) infos_.resize(std::max<size_t>(needed, infos_.size() * 2));
            infos_[task.dir_id] = info;
        }
        progress_.files.fetch_add(files);
        progress_.dirs.fetch_add(dirs);
        progress_.dirs_queued.fetch_add(found.size());
        progress_.dirs_done.fetch_add(1);
    }

    const Options& opt_;
    Progress& progress_;
    std::unordered_set<std::string> skip_names_, stop_paths_, app_paths_;
    std::vector<Arena> arenas_;
    std::mutex m_;
    std::condition_variable cv_;
    std::vector<Task> stack_;
    size_t active_ = 0;
    std::atomic<uint32_t> next_dir_{0};
    std::mutex info_m_;
    std::vector<DirInfo> infos_;
};

}  // namespace

std::shared_ptr<engine::IndexStore> build(const Options& opt, Progress& progress, int64_t now_ms) {
    Scanner s(opt, progress);
    auto store = s.run(now_ms);
    progress.finished = true;
    return store;
}

}  // namespace scan
