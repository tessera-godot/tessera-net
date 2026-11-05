// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "tessera/net/entity_id.hpp"

namespace tessera::net {

/// Number of slots in one page. A multiple of 64 so the bitsets are whole words.
inline constexpr uint32_t kPageSlots = 4096;

/// Settings of one entity table.
struct TableConfig {
    uint32_t shard = 0;  ///< Shard written into every id this table creates.
    uint32_t type = 0;   ///< Entity type written into every id this table creates.
    /// Upper bound on the number of slots; creation fails when it is reached. Mainly for tests.
    uint32_t max_slots = EntityId::kMaxIndex + 1;
};

/// Where an entity lives inside a table.
struct SlotLocation {
    uint32_t page;
    uint32_t slot;
};

/// Storage for all entities of one type, laid out as struct of arrays in fixed-size pages.
///
/// `PageColumns` is a struct that holds one array of `kPageSlots` elements per column (position,
/// health, ...). Pages are allocated on demand and never move, slots keep their index for the life
/// of the entity, and a generation counter per slot makes stale ids detectable.
template <class PageColumns>
class EntityTable {
public:
    explicit EntityTable(TableConfig config) : config_(config) {
        config_.max_slots = std::min(config_.max_slots, EntityId::kMaxIndex + 1);
    }

    /// Creates an entity and returns its id, or `EntityId::Null()` when the table is full.
    EntityId Create() {
        uint32_t index;
        if (!free_slots_.empty()) {
            index = free_slots_.back();
            free_slots_.pop_back();
        } else {
            if (next_new_slot_ >= config_.max_slots) {
                return EntityId::Null();
            }
            index = next_new_slot_++;
            if (index / kPageSlots >= pages_.size()) {
                pages_.push_back(std::make_unique<Page>());
            }
        }
        Page& page = *pages_[index / kPageSlots];
        const uint32_t slot = index % kPageSlots;
        page.alive[slot >> 6] |= 1ull << (slot & 63);
        ++alive_count_;
        return EntityId::Make(config_.shard, config_.type, index, page.generation[slot]);
    }

    /// Destroys an entity immediately. Returns false for null, foreign, stale or already destroyed ids.
    bool Destroy(EntityId id) {
        const std::optional<SlotLocation> location = Find(id);
        if (!location) {
            return false;
        }
        Page& page = *pages_[location->page];
        const uint64_t bit = 1ull << (location->slot & 63);
        page.alive[location->slot >> 6] &= ~bit;
        page.dirty[location->slot >> 6] &= ~bit;
        --alive_count_;
        const uint32_t next_generation = page.generation[location->slot] + 1;
        if (next_generation >= EntityId::kRetiredGeneration) {
            // The generation counter is exhausted: never reuse this slot, so no old id can match it.
            page.generation[location->slot] = EntityId::kRetiredGeneration;
            ++retired_count_;
        } else {
            page.generation[location->slot] = next_generation;
            free_slots_.push_back(id.index());
        }
        return true;
    }

    /// Number of slots permanently taken out of use because their generation counter ran out.
    size_t RetiredSlotCount() const noexcept { return retired_count_; }

    /// Flags a live entity as changed. Ignores null, foreign and stale ids.
    void MarkDirty(EntityId id) {
        const std::optional<SlotLocation> location = Find(id);
        if (location) {
            pages_[location->page]->dirty[location->slot >> 6] |= 1ull << (location->slot & 63);
        }
    }

    /// Calls `fn(slot)` for every flagged slot of a page and clears the flags. Returns how many were visited.
    template <class F>
    size_t ConsumeDirtyInPage(uint32_t page, F&& fn) {
        Page& p = *pages_[page];
        size_t visited = 0;
        for (uint32_t word = 0; word < kPageSlots / 64; ++word) {
            uint64_t bits = p.dirty[word];
            p.dirty[word] = 0;
            while (bits != 0) {
                fn(word * 64 + static_cast<uint32_t>(std::countr_zero(bits)));
                bits &= bits - 1;
                ++visited;
            }
        }
        return visited;
    }

    bool IsAlive(EntityId id) const { return Find(id).has_value(); }

    size_t AliveCount() const noexcept { return alive_count_; }

    /// Locates a live entity; empty when the id is not (or no longer) valid for this table.
    std::optional<SlotLocation> Find(EntityId id) const {
        if (id.IsNull() || id.shard() != config_.shard || id.type() != config_.type) {
            return std::nullopt;
        }
        const uint32_t index = id.index();
        if (index >= next_new_slot_) {
            return std::nullopt;
        }
        const uint32_t page_index = index / kPageSlots;
        const uint32_t slot = index % kPageSlots;
        const Page& page = *pages_[page_index];
        const bool alive = (page.alive[slot >> 6] >> (slot & 63)) & 1ull;
        if (!alive || page.generation[slot] != id.generation()) {
            return std::nullopt;
        }
        return SlotLocation{page_index, slot};
    }

    uint32_t PageCount() const noexcept { return static_cast<uint32_t>(pages_.size()); }

    /// Size in bytes of one page, columns included. Subtract `sizeof(PageColumns)` and divide by
    /// `kPageSlots` to get the per-entity bookkeeping cost.
    static constexpr size_t PageBytes() noexcept { return sizeof(Page); }

    PageColumns& Columns(uint32_t page) noexcept { return pages_[page]->columns; }
    const PageColumns& Columns(uint32_t page) const noexcept { return pages_[page]->columns; }

    /// Id of the occupant of a slot (valid only while that slot is alive).
    EntityId IdOf(uint32_t page, uint32_t slot) const {
        return EntityId::Make(config_.shard, config_.type, page * kPageSlots + slot,
                              pages_[page]->generation[slot]);
    }

    /// Calls `fn(slot)` for every live slot of a page, skipping empty slots a word at a time.
    template <class F>
    void ForEachAliveInPage(uint32_t page, F&& fn) const {
        const Page& p = *pages_[page];
        for (uint32_t word = 0; word < kPageSlots / 64; ++word) {
            uint64_t bits = p.alive[word];
            while (bits != 0) {
                fn(word * 64 + static_cast<uint32_t>(std::countr_zero(bits)));
                bits &= bits - 1;
            }
        }
    }

private:
    struct Page {
        PageColumns columns;
        uint32_t generation[kPageSlots];
        uint64_t alive[kPageSlots / 64];
        uint64_t dirty[kPageSlots / 64];
    };

    TableConfig config_;
    std::vector<std::unique_ptr<Page>> pages_;
    std::vector<uint32_t> free_slots_;
    uint32_t next_new_slot_ = 0;
    size_t alive_count_ = 0;
    size_t retired_count_ = 0;
};

}  // namespace tessera::net
