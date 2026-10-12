#pragma once

// The in-memory index: every file and folder as parallel arrays
// (structure-of-arrays), names in two contiguous blobs (original UTF-8 for
// display, folded for matching). Entries are stored in depth-first preorder,
// so a folder's whole subtree is the contiguous range [i, subtree_end(i)) and
// full paths are rebuilt on demand from the parent chain.
//
// A store is immutable once built; refreshes build a new one and swap the
// shared_ptr, so searches never need locks.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/fileio.h"

namespace engine {

constexpr uint32_t kNone = 0xFFFFFFFFu;

enum EntryFlag : uint8_t {
    kDir = 1,
    kHidden = 2,
    kSystem = 4,
    kApp = 8,      // Start Menu shortcut, desktop .lnk, ...
    kLink = 16,    // symlink/junction (not descended)
    kRoot = 32,    // an indexed location; its stored name is the full path
};

class IndexStore {
public:
    size_t size() const noexcept { return parent_.size(); }
    bool empty() const noexcept { return parent_.empty(); }

    uint32_t parent(uint32_t i) const noexcept { return parent_[i]; }
    uint8_t flags(uint32_t i) const noexcept { return flags_[i]; }
    bool is_dir(uint32_t i) const noexcept { return flags_[i] & kDir; }
    bool is_root(uint32_t i) const noexcept { return flags_[i] & kRoot; }
    uint64_t file_size(uint32_t i) const noexcept { return size_[i]; }
    int64_t mtime(uint32_t i) const noexcept { return mtime_[i]; }
    uint8_t depth(uint32_t i) const noexcept { return depth_[i]; }
    uint64_t mask(uint32_t i) const noexcept { return mask_[i]; }
    uint32_t subtree_end(uint32_t i) const noexcept { return end_[i]; }

    // Display name (the basename, also for roots).
    std::string_view name(uint32_t i) const noexcept;
    // Stored name: the basename, or the full path for roots.
    std::string_view stored_name(uint32_t i) const noexcept {
        return std::string_view(names_).substr(name_off_[i], name_off_[i + 1] - name_off_[i]);
    }
    // Folded basename (what matching runs on).
    std::string_view folded(uint32_t i) const noexcept {
        return std::string_view(folded_).substr(fold_off_[i], fold_off_[i + 1] - fold_off_[i] - 1);
    }

    std::string path(uint32_t i) const;         // full native path
    std::string parent_path(uint32_t i) const;  // containing folder
    std::vector<uint32_t> roots() const;
    uint32_t find_path(std::string_view path) const;  // kNone if not indexed
    // The root an entry belongs to.
    uint32_t root_of(uint32_t i) const noexcept;

    int64_t built_at() const noexcept { return built_at_; }
    uint64_t count_files() const noexcept;
    uint64_t count_dirs() const noexcept;
    uint64_t memory_bytes() const noexcept;

    // Raw access for the matcher: folded names separated by '\n'; entry i
    // occupies [fold_offsets()[i], fold_offsets()[i + 1] - 1).
    const char* folded_blob() const noexcept { return folded_.data(); }
    const uint32_t* fold_offsets() const noexcept { return fold_off_.data(); }

    bool save(const std::string& file, std::string* error = nullptr) const;
    static std::shared_ptr<IndexStore> load(const std::string& file, std::string* error = nullptr);

    // New store without the subtree of `root` (used by `pearch remove`).
    std::shared_ptr<IndexStore> without_root(uint32_t root) const;
    // New store with the entries of `b` appended after `a`.
    static std::shared_ptr<IndexStore> merge(const IndexStore& a, const IndexStore& b);

private:
    friend class IndexBuilder;
    void finalize();

    std::vector<uint32_t> parent_;
    std::vector<uint32_t> name_off_{0};  // size()+1
    std::vector<uint32_t> fold_off_{0};  // size()+1
    std::vector<uint8_t> flags_;
    std::vector<uint64_t> size_;
    std::vector<int64_t> mtime_;
    std::string names_;
    std::string folded_;
    int64_t built_at_ = 0;

    // Derived at finalize()/load(), not persisted.
    std::vector<uint32_t> end_;
    std::vector<uint8_t> depth_;
    std::vector<uint64_t> mask_;
};

// Appends entries in depth-first preorder (a parent before its children,
// each subtree contiguous).
class IndexBuilder {
public:
    IndexBuilder();
    void reserve(size_t entries, size_t name_bytes);
    uint32_t add_root(std::string_view full_path, int64_t mtime);
    uint32_t add(uint32_t parent, std::string_view name, uint8_t flags, uint64_t size, int64_t mtime);
    // `folded` must equal text::fold(name) (or of the basename for roots).
    uint32_t add_raw(uint32_t parent, std::string_view stored_name, std::string_view folded,
                     uint8_t flags, uint64_t size, int64_t mtime);
    size_t size() const noexcept { return store_->size(); }
    std::shared_ptr<IndexStore> finish(int64_t built_at);

private:
    std::shared_ptr<IndexStore> store_;
};

}  // namespace engine
