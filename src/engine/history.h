#pragma once

// Remembers what you open through pearch ("frecency": how often and how
// recently) so those items float to the top of later searches and fill the
// Recent view.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

class IndexStore;

// Ranking boost per entry id, resolved against one IndexStore.
struct Boosts {
    std::unordered_map<uint32_t, int32_t> by_id;
    std::vector<uint32_t> ranked;  // most relevant first
    int32_t get(uint32_t id) const {
        auto it = by_id.find(id);
        return it == by_id.end() ? 0 : it->second;
    }
};

class History {
public:
    bool load(const std::string& file);
    bool save(const std::string& file) const;
    void record(const std::string& path, int64_t now_ms);
    size_t size() const noexcept { return items_.size(); }

    std::shared_ptr<const Boosts> resolve(const IndexStore& store, int64_t now_ms) const;

    static int32_t frecency(uint32_t count, int64_t last_ms, int64_t now_ms) noexcept;

private:
    struct Item {
        uint32_t count = 0;
        int64_t last = 0;
    };
    std::unordered_map<std::string, Item> items_;
};

}  // namespace engine
