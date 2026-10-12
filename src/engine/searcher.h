#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "engine/history.h"
#include "engine/index_store.h"
#include "engine/query.h"
#include "engine/thread_pool.h"

namespace engine {

enum class SortMode : uint8_t { Relevance, Name, Modified, Size };
const char* sort_mode_name(SortMode m) noexcept;

struct SearchRequest {
    std::string text;
    TypeFilter tab = TypeFilter::All;  // overrides file:/folder:/app: in the text
    SortMode sort = SortMode::Relevance;
    uint32_t scope = kNone;            // only search inside this folder (subtree)
    int64_t now_ms = 0;
    std::shared_ptr<const Boosts> boosts;
};

struct Hit {
    int32_t score;
    uint32_t id;
};

class SearchResult {
public:
    std::shared_ptr<const IndexStore> store;
    Query query;
    SortMode sort = SortMode::Relevance;
    bool recent = false;       // empty query: recent items / all apps
    bool fuzzy = false;        // includes fuzzy (letters-in-order) matches
    double ms = 0;             // time spent searching
    uint64_t generation = 0;

    size_t size() const noexcept { return hits_.size(); }
    bool empty() const noexcept { return hits_.empty(); }
    // k-th result in rank order (sorts lazily as you scroll).
    uint32_t id(size_t k);
    int32_t score(size_t k);
    void ensure_sorted(size_t n);

private:
    // Top-k selection spread over the pool (for very large result sets).
    void select_top(ThreadPool& pool, size_t k);
    friend class Searcher;
    bool before(const Hit& a, const Hit& b) const noexcept;
    std::vector<Hit> hits_;
    size_t sorted_ = 0;
};

class Searcher {
public:
    explicit Searcher(ThreadPool& pool) : pool_(pool) {}
    SearchResult search(std::shared_ptr<const IndexStore> store, const SearchRequest& req);

private:
    const std::vector<uint32_t>& app_ids(const IndexStore& s);

    ThreadPool& pool_;
    const IndexStore* apps_for_ = nullptr;
    std::vector<uint32_t> apps_;
    // Incremental narrowing: matches of the previous query.
    std::shared_ptr<const IndexStore> last_store_;
    Query last_query_;
    uint32_t last_scope_ = kNone;
    std::vector<uint32_t> last_strict_;
    bool last_valid_ = false;
};

// Runs searches on a background thread; only the newest request matters.
class AsyncSearcher {
public:
    explicit AsyncSearcher(ThreadPool& pool, std::function<void()> on_done = {});
    ~AsyncSearcher();
    uint64_t submit(std::shared_ptr<const IndexStore> store, SearchRequest req);
    // The newest finished result, if one arrived since the last call.
    std::unique_ptr<SearchResult> take();
    bool busy() const noexcept { return busy_.load(); }

private:
    void loop();
    Searcher searcher_;
    std::function<void()> on_done_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::shared_ptr<const IndexStore> pending_store_;
    std::optional<SearchRequest> pending_;
    uint64_t generation_ = 0;
    std::unique_ptr<SearchResult> done_;
    std::atomic<bool> busy_{false};
    std::thread thread_;
};

}  // namespace engine
