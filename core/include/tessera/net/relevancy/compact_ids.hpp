// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <vector>

namespace tessera::net {

/// Hands out small ids for the entities one connection can see. Small ids are cheap to send, so the
/// network layer uses them in place of the 64-bit entity id.
///
/// An id that is released is not handed out again until `BeginTick` is called. That gives the client
/// one full tick to process "this entity left" before the same number means a different entity.
class CompactIdAllocator {
public:
    /// Throws std::invalid_argument if `capacity` is 0.
    explicit CompactIdAllocator(uint32_t capacity);

    /// The lowest free id in [0, capacity), or `kInvalidCompactId` if there is none.
    uint32_t Acquire();

    /// Gives an id back. Returns false if the id is out of range, not in use, or already released.
    /// The id becomes available again after the next `BeginTick`.
    bool Release(uint32_t id);

    /// Makes the ids released since the previous call available again. Call once per tick.
    void BeginTick();

    /// Ids handed out and not yet available again (ids waiting for `BeginTick` are counted).
    uint32_t InUse() const noexcept {
        return in_use_;
    }

    uint32_t Capacity() const noexcept {
        return capacity_;
    }

private:
    uint32_t capacity_;
    uint32_t in_use_ = 0;
    uint32_t lowest_word_ = 0;      // No word before this one has a free bit.
    std::vector<uint64_t> used_;    // 1 = handed out and not yet available again.
    std::vector<uint64_t> pending_; // 1 = released, waiting for BeginTick.
    std::vector<uint32_t> released_;
};

} // namespace tessera::net
