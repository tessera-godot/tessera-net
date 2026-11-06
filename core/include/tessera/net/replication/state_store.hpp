// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tessera/net/entity_id.hpp"
#include "tessera/net/entity_table.hpp"
#include "tessera/net/job_system.hpp"
#include "tessera/net/replication/property_codecs.hpp"
#include "tessera/net/replication/property_schema.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace tessera::net {

/// The quantized state of every replicated entity as of the last `Refresh`. A game keeps its entities in
/// `EntityTable`s; before replicating, it calls `Refresh` on each table, and the store reads the columns of
/// the entities flagged with `MarkDirty`, turns them into quantized words (once, whatever the number of
/// connections) and reports the ones whose words actually changed.
///
/// A game must call `MarkDirty` after it creates an entity and after it changes a replicated column. An
/// entity that was never flagged has no state here and is not replicated.
///
/// Thread safety: `Refresh` runs its pages in parallel and must not overlap any other call. The reading
/// calls may run in parallel with each other.
class StateStore {
public:
    /// The schema must outlive the store and must not be moved.
    explicit StateStore(const ReplicationSchema& schema) : schema_(schema) {}

    /// Quantizes the flagged entities of one table (clearing the flags), adds those whose state changed to
    /// the list of changes and returns how many that was. The table's type decides which schema is used; a
    /// table whose type has no schema is not stored and its flagged entities only add to
    /// `UnreplicatedCount`.
    template <class Columns> size_t Refresh(EntityTable<Columns>& table, IJobSystem& jobs);

    /// The quantized words of a live entity that has been refreshed, or nullptr if it has not or if the id is
    /// stale (the slot now holds another entity that has been refreshed). The store does not learn about
    /// destruction by itself: whoever uses it drops destroyed entities through the visibility events.
    const uint32_t* Find(EntityId id) const noexcept;

    /// Entities whose state changed since `ClearChanged`: ascending by index within one `Refresh` call, and
    /// the calls in the order they were made.
    std::span<const EntityId> Changed() const noexcept {
        return changed_;
    }
    void ClearChanged() noexcept {
        changed_.clear();
    }

    /// How many flagged entities belonged to a type without a schema.
    uint64_t UnreplicatedCount() const noexcept {
        return unreplicated_;
    }

    /// Bytes held by the stored states and the list of changes.
    size_t MemoryBytes() const noexcept;

private:
    static constexpr uint32_t kNoGeneration = 0xFFFFFFFFu;

    struct Page {
        std::vector<uint32_t> words;      // kPageSlots * words_per_entity
        std::vector<uint32_t> generation; // kPageSlots, kNoGeneration until the slot is first stored
    };
    struct TypeStore {
        const TypeSchema* schema = nullptr;
        std::vector<std::unique_ptr<Page>> pages;
    };

    TypeStore& StoreFor(uint32_t type, size_t pages_needed);

    const ReplicationSchema& schema_;
    std::array<std::unique_ptr<TypeStore>, 256> stores_;
    std::vector<EntityId> changed_;
    std::vector<std::vector<uint32_t>>
        page_changes_; // scratch: changed slots of each page of the current table
    uint64_t unreplicated_ = 0;
};

template <class Columns> size_t StateStore::Refresh(EntityTable<Columns>& table, IJobSystem& jobs) {
    const uint32_t type = table.Config().type;
    const uint32_t page_count = table.PageCount();
    const TypeSchema* type_schema = schema_.Find(type);
    if (type_schema == nullptr) {
        for (uint32_t page = 0; page < page_count; ++page) {
            unreplicated_ += table.ConsumeDirtyInPage(page, [](uint32_t) {});
        }
        return 0;
    }

    // Pages are allocated before the parallel part, so the workers only read and write memory that exists.
    TypeStore& store = StoreFor(type, page_count);
    if (page_changes_.size() < page_count) {
        page_changes_.resize(page_count);
    }
    const uint32_t words_per_entity = type_schema->words;

    jobs.ParallelFor(page_count, 1, [&](uint32_t begin, uint32_t end, uint32_t) {
        for (uint32_t page_index = begin; page_index < end; ++page_index) {
            Page& page = *store.pages[page_index];
            std::vector<uint32_t>& changes = page_changes_[page_index];
            changes.clear();
            const std::byte* columns = reinterpret_cast<const std::byte*>(&table.Columns(page_index));
            uint32_t scratch[ReplicationSchema::kMaxWordsPerEntity];
            table.ConsumeDirtyInPage(page_index, [&](uint32_t slot) {
                for (const PropertyDescriptor& property : type_schema->properties) {
                    QuantizeProperty(property, columns, slot, scratch + property.word_offset);
                }
                const uint32_t generation = table.IdOf(page_index, slot).generation();
                uint32_t* stored = page.words.data() + static_cast<size_t>(slot) * words_per_entity;
                if (page.generation[slot] != generation ||
                    std::memcmp(stored, scratch, words_per_entity * sizeof(uint32_t)) != 0) {
                    std::memcpy(stored, scratch, words_per_entity * sizeof(uint32_t));
                    page.generation[slot] = generation;
                    changes.push_back(slot);
                }
            });
        }
    });

    size_t total = 0;
    for (uint32_t page_index = 0; page_index < page_count; ++page_index) {
        for (const uint32_t slot : page_changes_[page_index]) {
            changed_.push_back(table.IdOf(page_index, slot));
        }
        total += page_changes_[page_index].size();
    }
    return total;
}

} // namespace tessera::net
