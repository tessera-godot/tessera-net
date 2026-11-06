// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tessera::net {

/// One entity a connection can see, and the compact id it has on that connection.
struct VisibleEntry {
    uint64_t raw; ///< `EntityId::raw()`
    uint32_t compact_id;
};

/// The entities one connection can see, kept in a vector ordered by `raw`. Looking one up is a binary
/// search, walking the whole set is a straight read of memory, and a batch of changes is folded in with
/// one pass. A hash table does the first well and the others badly, and the engine needs the others.
class VisibleSet {
public:
    size_t Size() const noexcept {
        return entries_.size();
    }

    /// The entry for `raw`, or nullptr if the entity is not in the set. The pointer is valid until the
    /// next `Apply` or `Clear`.
    const VisibleEntry* Find(uint64_t raw) const noexcept;

    bool Contains(uint64_t raw) const noexcept {
        return Find(raw) != nullptr;
    }

    /// Every entry, ascending by `raw`.
    std::span<const VisibleEntry> Entries() const noexcept {
        return entries_;
    }

    /// Takes out `leaving` and puts in `entering`, in a single pass. `leaving` is ascending and every
    /// value is in the set; `entering` is ascending by `raw` and no value is in the set yet (the set stays
    /// ordered only if these hold; they are checked in Debug builds).
    void Apply(std::span<const uint64_t> leaving, std::span<const VisibleEntry> entering);

    void Clear() noexcept {
        entries_.clear();
    }

private:
    std::vector<VisibleEntry> entries_;
    std::vector<VisibleEntry> scratch_; // reused by Apply so it stops allocating once it has grown
};

} // namespace tessera::net
