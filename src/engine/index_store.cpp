#include "engine/index_store.h"

#include <algorithm>
#include <cstring>
#include <thread>

#include "core/hardware.h"

#include "core/text.h"

namespace engine {

namespace {

constexpr char kMagic[8] = {'P', 'E', 'A', 'R', 'C', 'H', 'I', 'X'};
constexpr uint32_t kVersion = 1;
constexpr uint32_t kEndianTag = 0x01020304u;
constexpr uint64_t kTrailer = 0x444E454843524145ull;  // "EARCHEND"
constexpr uint64_t kMaxEntries = 200'000'000ull;

std::string_view basename_view(std::string_view p) {
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.remove_suffix(1);
    size_t pos = p.find_last_of("/\\");
    if (pos == std::string_view::npos || p.size() == 1) return p;
    return p.substr(pos + 1);
}

template <typename T>
bool write_vec(FILE* f, const std::vector<T>& v) {
    return v.empty() || std::fwrite(v.data(), sizeof(T), v.size(), f) == v.size();
}

template <typename T>
bool read_vec(FILE* f, std::vector<T>& v, size_t n) {
    v.resize(n);
    return n == 0 || std::fread(v.data(), sizeof(T), n, f) == n;
}

template <typename T>
bool write_pod(FILE* f, const T& v) { return std::fwrite(&v, sizeof v, 1, f) == 1; }
template <typename T>
bool read_pod(FILE* f, T& v) { return std::fread(&v, sizeof v, 1, f) == 1; }

void set_error(std::string* err, const char* msg) { if (err) *err = msg; }

// Checks the structural invariants every consumer relies on.
bool validate(const std::vector<uint32_t>& parent, const std::vector<uint32_t>& name_off,
              const std::vector<uint32_t>& fold_off, const std::vector<uint8_t>& flags,
              const std::string& names, const std::string& folded) {
    const size_t n = parent.size();
    if (name_off.size() != n + 1 || fold_off.size() != n + 1 || flags.size() != n) return false;
    if (name_off[0] != 0 || fold_off[0] != 0) return false;
    if (name_off[n] != names.size() || fold_off[n] != folded.size()) return false;
    std::vector<uint32_t> stack;
    for (size_t i = 0; i < n; ++i) {
        if (name_off[i + 1] < name_off[i]) return false;
        if (fold_off[i + 1] <= fold_off[i]) return false;
        if (folded[fold_off[i + 1] - 1] != '\n') return false;
        uint32_t p = parent[i];
        bool root = (flags[i] & kRoot) != 0;
        if (root != (p == kNone)) return false;
        // Depth-first preorder: the parent must be on the open-folder stack.
        if (p == kNone) {
            stack.clear();
        } else {
            while (!stack.empty() && stack.back() != p) stack.pop_back();
            if (stack.empty()) return false;
        }
        if (flags[i] & kDir) stack.push_back(static_cast<uint32_t>(i));
    }
    return true;
}

}  // namespace

std::string_view IndexStore::name(uint32_t i) const noexcept {
    std::string_view s = stored_name(i);
    return (flags_[i] & kRoot) ? basename_view(s) : s;
}

std::string IndexStore::path(uint32_t i) const {
    uint32_t chain[256];
    size_t depth = 0;
    std::vector<uint32_t> deep;  // only used for absurdly deep trees
    for (uint32_t p = i; p != kNone; p = parent_[p]) {
        if (depth < 256) chain[depth++] = p;
        else deep.push_back(p);
    }
    if (!deep.empty()) {
        std::vector<uint32_t> all(chain, chain + depth);
        all.insert(all.end(), deep.begin(), deep.end());
        std::string out(stored_name(all.back()));
        for (size_t k = all.size() - 1; k-- > 0;) {
            if (!out.empty() && out.back() != '/' && out.back() != '\\') out += fileio::kPathSep;
            out += stored_name(all[k]);
        }
        return out;
    }
    std::string out(stored_name(chain[depth - 1]));
    for (size_t k = depth - 1; k-- > 0;) {
        if (!out.empty() && out.back() != '/' && out.back() != '\\') out += fileio::kPathSep;
        out += stored_name(chain[k]);
    }
    return out;
}

std::string IndexStore::parent_path(uint32_t i) const {
    uint32_t p = parent_[i];
    if (p == kNone) return fileio::parent_of(std::string(stored_name(i)));
    return path(p);
}

std::vector<uint32_t> IndexStore::roots() const {
    std::vector<uint32_t> out;
    for (uint32_t r = 0; r < size(); r = end_[r]) out.push_back(r);
    return out;
}

uint32_t IndexStore::root_of(uint32_t i) const noexcept {
    while (parent_[i] != kNone) i = parent_[i];
    return i;
}

uint32_t IndexStore::find_path(std::string_view path_in) const {
    std::string path(path_in);
    // The most specific location wins (e.g. Start Menu inside the profile).
    std::vector<uint32_t> candidates;
    for (uint32_t r = 0; r < size(); r = end_[r])
        if (fileio::is_within(path, std::string(stored_name(r)))) candidates.push_back(r);
    std::sort(candidates.begin(), candidates.end(),
              [&](uint32_t a, uint32_t b) { return stored_name(a).size() > stored_name(b).size(); });
    for (uint32_t r : candidates) {
        std::string root(stored_name(r));
        uint32_t cur = r;
        std::string_view rest = std::string_view(path).substr(root.size());
        size_t i = 0;
        while (i < rest.size() && cur != kNone) {
            while (i < rest.size() && (rest[i] == '/' || rest[i] == '\\')) ++i;
            if (i >= rest.size()) break;
            size_t j = i;
            while (j < rest.size() && rest[j] != '/' && rest[j] != '\\') ++j;
            std::string_view comp = rest.substr(i, j - i);
            std::string fc = text::fold(comp);
            uint32_t exact = kNone, loose = kNone;
            for (uint32_t c = cur + 1; c < end_[cur]; c = end_[c]) {
                if (stored_name(c) == comp) { exact = c; break; }
                if (loose == kNone && folded(c) == fc) loose = c;
            }
            cur = exact != kNone ? exact : loose;
            i = j;
        }
        if (cur != kNone) return cur;
    }
    return kNone;
}

uint64_t IndexStore::count_files() const noexcept {
    uint64_t n = 0;
    for (uint8_t f : flags_) n += (f & kDir) ? 0 : 1;
    return n;
}

uint64_t IndexStore::count_dirs() const noexcept { return size() - count_files(); }

uint64_t IndexStore::memory_bytes() const noexcept {
    return parent_.capacity() * 4 + name_off_.capacity() * 4 + fold_off_.capacity() * 4 +
           flags_.capacity() + size_.capacity() * 8 + mtime_.capacity() * 8 + names_.capacity() +
           folded_.capacity() + end_.capacity() * 4 + depth_.capacity() + mask_.capacity() * 8;
}

void IndexStore::finalize() {
    const size_t n = size();
    end_.resize(n);
    depth_.resize(n);
    mask_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        end_[i] = static_cast<uint32_t>(i + 1);
        uint32_t p = parent_[i];
        depth_[i] = p == kNone ? 0 : static_cast<uint8_t>(std::min(255, depth_[p] + 1));
    }
    // Character masks are independent per entry: spread them over all cores.
    auto masks = [this](size_t a, size_t b) {
        for (size_t i = a; i < b; ++i) mask_[i] = text::char_mask(folded(static_cast<uint32_t>(i)));
    };
    const unsigned workers = n >= 200000 ? std::min(hw::search_threads(), 32u) : 1;
    if (workers <= 1) {
        masks(0, n);
    } else {
        std::vector<std::thread> pool;
        for (unsigned w = 0; w < workers; ++w) pool.emplace_back(masks, n * w / workers, n * (w + 1) / workers);
        for (auto& t : pool) t.join();
    }
    for (size_t i = n; i-- > 0;) {
        uint32_t p = parent_[i];
        if (p != kNone && end_[i] > end_[p]) end_[p] = end_[i];
    }
}

bool IndexStore::save(const std::string& file, std::string* error) const {
    std::string tmp = file + ".tmp";
    FILE* f = fileio::open(tmp, "wb");
    if (!f) { set_error(error, "cannot write index file"); return false; }
    std::vector<char> buffer(1 << 20);
    std::setvbuf(f, buffer.data(), _IOFBF, buffer.size());
    const uint64_t count = size();
    const uint64_t names_bytes = names_.size();
    const uint64_t folded_bytes = folded_.size();
    bool ok = std::fwrite(kMagic, 1, sizeof kMagic, f) == sizeof kMagic;
    ok = ok && write_pod(f, kVersion) && write_pod(f, kEndianTag) && write_pod(f, count) &&
         write_pod(f, names_bytes) && write_pod(f, folded_bytes) && write_pod(f, built_at_);
    ok = ok && write_vec(f, parent_) && write_vec(f, name_off_) && write_vec(f, fold_off_) &&
         write_vec(f, flags_) && write_vec(f, size_) && write_vec(f, mtime_);
    ok = ok && std::fwrite(names_.data(), 1, names_.size(), f) == names_.size();
    ok = ok && std::fwrite(folded_.data(), 1, folded_.size(), f) == folded_.size();
    ok = ok && write_pod(f, kTrailer);
    ok = (std::fflush(f) == 0) && ok;
    ok = (std::fclose(f) == 0) && ok;
    if (!ok || !fileio::replace(tmp, file)) {
        fileio::remove_file(tmp);
        set_error(error, "failed to write index file");
        return false;
    }
    return true;
}

std::shared_ptr<IndexStore> IndexStore::load(const std::string& file, std::string* error) {
    FILE* f = fileio::open(file, "rb");
    if (!f) { set_error(error, "no index yet"); return nullptr; }
    std::vector<char> buffer(1 << 20);
    std::setvbuf(f, buffer.data(), _IOFBF, buffer.size());
    auto s = std::make_shared<IndexStore>();
    char magic[8];
    uint32_t version = 0, endian = 0;
    uint64_t count = 0, names_bytes = 0, folded_bytes = 0;
    bool ok = std::fread(magic, 1, sizeof magic, f) == sizeof magic &&
              std::memcmp(magic, kMagic, sizeof magic) == 0;
    ok = ok && read_pod(f, version) && version == kVersion && read_pod(f, endian) &&
         endian == kEndianTag && read_pod(f, count) && read_pod(f, names_bytes) &&
         read_pod(f, folded_bytes) && read_pod(f, s->built_at_);
    ok = ok && count <= kMaxEntries && names_bytes < 0xFFFFFFFFull && folded_bytes < 0xFFFFFFFFull;
    if (ok) {
        ok = read_vec(f, s->parent_, count) && read_vec(f, s->name_off_, count + 1) &&
             read_vec(f, s->fold_off_, count + 1) && read_vec(f, s->flags_, count) &&
             read_vec(f, s->size_, count) && read_vec(f, s->mtime_, count);
    }
    if (ok) {
        s->names_.resize(names_bytes);
        s->folded_.resize(folded_bytes);
        ok = std::fread(s->names_.data(), 1, names_bytes, f) == names_bytes &&
             std::fread(s->folded_.data(), 1, folded_bytes, f) == folded_bytes;
        uint64_t trailer = 0;
        ok = ok && read_pod(f, trailer) && trailer == kTrailer;
    }
    std::fclose(f);
    if (!ok || !validate(s->parent_, s->name_off_, s->fold_off_, s->flags_, s->names_, s->folded_)) {
        set_error(error, "index file is damaged or from another version");
        return nullptr;
    }
    s->finalize();
    return s;
}

std::shared_ptr<IndexStore> IndexStore::without_root(uint32_t root) const {
    IndexBuilder b;
    b.reserve(size(), names_.size());
    std::vector<uint32_t> map(size(), kNone);
    const uint32_t skip_end = end_[root];
    for (uint32_t i = 0; i < size(); ++i) {
        if (i == root) { i = skip_end - 1; continue; }
        uint32_t p = parent_[i] == kNone ? kNone : map[parent_[i]];
        map[i] = b.add_raw(p, stored_name(i), folded(i), flags_[i], size_[i], mtime_[i]);
    }
    return b.finish(built_at_);
}

std::shared_ptr<IndexStore> IndexStore::merge(const IndexStore& a, const IndexStore& b) {
    IndexBuilder out;
    out.reserve(a.size() + b.size(), a.names_.size() + b.names_.size());
    for (const IndexStore* s : {&a, &b}) {
        const uint32_t base = static_cast<uint32_t>(out.size());
        for (uint32_t i = 0; i < s->size(); ++i) {
            uint32_t p = s->parent_[i] == kNone ? kNone : s->parent_[i] + base;
            out.add_raw(p, s->stored_name(i), s->folded(i), s->flags_[i], s->size_[i], s->mtime_[i]);
        }
    }
    return out.finish(std::max(a.built_at_, b.built_at_));
}

IndexBuilder::IndexBuilder() : store_(std::make_shared<IndexStore>()) {}

void IndexBuilder::reserve(size_t entries, size_t name_bytes) {
    auto& s = *store_;
    s.parent_.reserve(entries);
    s.name_off_.reserve(entries + 1);
    s.fold_off_.reserve(entries + 1);
    s.flags_.reserve(entries);
    s.size_.reserve(entries);
    s.mtime_.reserve(entries);
    s.names_.reserve(name_bytes);
    s.folded_.reserve(name_bytes + entries);
}

uint32_t IndexBuilder::add_root(std::string_view full_path, int64_t mtime) {
    std::string folded = text::fold(basename_view(full_path));
    return add_raw(kNone, full_path, folded, kDir | kRoot, 0, mtime);
}

uint32_t IndexBuilder::add(uint32_t parent, std::string_view name, uint8_t flags, uint64_t size,
                           int64_t mtime) {
    std::string folded = text::fold(name);
    return add_raw(parent, name, folded, static_cast<uint8_t>(flags & ~kRoot), size, mtime);
}

uint32_t IndexBuilder::add_raw(uint32_t parent, std::string_view stored_name,
                               std::string_view folded, uint8_t flags, uint64_t size,
                               int64_t mtime) {
    auto& s = *store_;
    if (s.names_.size() + stored_name.size() >= 0xFFFFFFF0ull ||
        s.folded_.size() + folded.size() + 1 >= 0xFFFFFFF0ull)
        return kNone;
    const auto id = static_cast<uint32_t>(s.parent_.size());
    s.parent_.push_back(parent);
    s.flags_.push_back(parent == kNone ? static_cast<uint8_t>(flags | kRoot | kDir) : flags);
    s.size_.push_back(size);
    s.mtime_.push_back(mtime);
    s.names_.append(stored_name);
    s.name_off_.push_back(static_cast<uint32_t>(s.names_.size()));
    // The folded blob uses '\n' as the entry separator, so it must not occur
    // inside a name (possible on Linux).
    size_t start = s.folded_.size();
    s.folded_.append(folded);
    for (size_t k = start; k < s.folded_.size(); ++k)
        if (s.folded_[k] == '\n' || s.folded_[k] == '\r') s.folded_[k] = '?';
    s.folded_.push_back('\n');
    s.fold_off_.push_back(static_cast<uint32_t>(s.folded_.size()));
    return id;
}

std::shared_ptr<IndexStore> IndexBuilder::finish(int64_t built_at) {
    auto s = std::move(store_);
    store_ = std::make_shared<IndexStore>();
    s->built_at_ = built_at;
    s->names_.shrink_to_fit();
    s->folded_.shrink_to_fit();
    if (!validate(s->parent_, s->name_off_, s->fold_off_, s->flags_, s->names_, s->folded_))
        return nullptr;
    s->finalize();
    return s;
}

}  // namespace engine
