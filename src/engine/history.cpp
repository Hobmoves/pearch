#include "engine/history.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/fileio.h"
#include "core/text.h"
#include "engine/index_store.h"

namespace engine {

namespace {
constexpr size_t kMaxItems = 500;
}

bool History::load(const std::string& file) {
    std::string data;
    if (!fileio::read_all(file, data)) return false;
    items_.clear();
    for (const auto& line : text::split(data, '\n')) {
        auto parts = text::split(line, '\t');
        if (parts.size() < 3 || parts[2].empty()) continue;
        Item it;
        it.count = static_cast<uint32_t>(std::strtoul(parts[0].c_str(), nullptr, 10));
        it.last = std::strtoll(parts[1].c_str(), nullptr, 10);
        std::string path = std::string(text::trim(parts[2]));
        if (it.count > 0 && !path.empty()) items_[path] = it;
    }
    return true;
}

bool History::save(const std::string& file) const {
    std::string out;
    for (const auto& [path, it] : items_) {
        out += std::to_string(it.count);
        out += '\t';
        out += std::to_string(it.last);
        out += '\t';
        out += path;
        out += '\n';
    }
    return fileio::write_atomic(file, out);
}

void History::record(const std::string& path, int64_t now_ms) {
    Item& it = items_[path];
    it.count += 1;
    it.last = now_ms;
    if (items_.size() > kMaxItems) {
        // Forget the least relevant entry.
        auto worst = std::min_element(items_.begin(), items_.end(), [&](const auto& a, const auto& b) {
            return frecency(a.second.count, a.second.last, now_ms) <
                   frecency(b.second.count, b.second.last, now_ms);
        });
        if (worst != items_.end() && worst->first != path) items_.erase(worst);
    }
}

int32_t History::frecency(uint32_t count, int64_t last_ms, int64_t now_ms) noexcept {
    double age_days = static_cast<double>(now_ms - last_ms) / 86400000.0;
    double decay = age_days < 1 ? 1.0 : age_days < 7 ? 0.8 : age_days < 30 ? 0.5 : age_days < 90 ? 0.3 : 0.15;
    double base = std::min(500.0, 160.0 * std::log2(1.0 + count));
    return static_cast<int32_t>(base * decay);
}

std::shared_ptr<const Boosts> History::resolve(const IndexStore& store, int64_t now_ms) const {
    auto b = std::make_shared<Boosts>();
    std::vector<std::pair<int32_t, uint32_t>> ranked;
    for (const auto& [path, it] : items_) {
        uint32_t id = store.find_path(path);
        if (id == kNone) continue;
        int32_t score = frecency(it.count, it.last, now_ms);
        b->by_id[id] = score;
        ranked.emplace_back(score, id);
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& c) { return a.first > c.first; });
    for (const auto& r : ranked) b->ranked.push_back(r.second);
    return b;
}

}  // namespace engine
