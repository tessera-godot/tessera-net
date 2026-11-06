// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tessera/net/entity_id.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace tessera::net {

/// For each entity, the connections that see it and the compact id it has on each. When an entity changes,
/// the work is then proportional to the connections that actually see it, not to every connection.
///
/// It is laid out like an entity table (a page of 4,096 entries for each range of entity indexes of each
/// type), so finding an entity is a few array reads and no hashing. The subscribers of one entity are a short
/// linked list in a shared pool of nodes; removed nodes are reused.
///
/// An entry is tied to the generation of the entity id it was added with, so a change to an entity that took
/// over the slot of an older one never reaches the older one's viewers.
///
/// Not thread safe: changes must run alone, and so must reads that overlap them. Reads may overlap each
/// other.
class SubscriptionIndex {
public:
    /// Records that `connection` sees `entity` as `compact_id`. The caller makes sure it was not added
    /// before. `connection` must fit in 16 bits.
    void Add(EntityId entity, uint32_t connection, uint32_t compact_id);

    /// Forgets that `connection` sees `entity`. Returns false if it was not recorded.
    bool Remove(EntityId entity, uint32_t connection);

    /// Calls `fn(connection, compact_id)` for every connection that sees `entity`.
    template <class F> void ForEach(EntityId entity, F&& fn) const {
        const uint32_t* head = Head(entity);
        if (head == nullptr) {
            return;
        }
        for (uint32_t at = *head; at != kNone; at = nodes_[at].next) {
            const Node& node = nodes_[at];
            if (node.generation == entity.generation()) {
                fn(static_cast<uint32_t>(node.connection), node.compact_id);
            }
        }
    }

    /// How many (entity, connection) pairs are recorded.
    size_t Count() const noexcept {
        return count_;
    }

    /// Bytes held by the pages and the pool of nodes.
    size_t MemoryBytes() const noexcept;

private:
    static constexpr uint32_t kNone = 0xFFFFFFFFu;
    static constexpr uint32_t kPageEntries = 4096;

    struct Node {
        uint32_t compact_id;
        uint16_t connection;
        uint16_t generation;
        uint32_t next;
    };
    using Page = std::vector<uint32_t>; // kPageEntries heads, kNone for an entity nobody sees

    const uint32_t* Head(EntityId entity) const noexcept;
    uint32_t* Head(EntityId entity, bool create);

    std::array<std::vector<std::unique_ptr<Page>>, 256> types_;
    std::vector<Node> nodes_;
    uint32_t free_ = kNone; // first reusable node
    size_t count_ = 0;
};

} // namespace tessera::net
