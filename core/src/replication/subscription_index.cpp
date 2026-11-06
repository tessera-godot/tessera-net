// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/replication/subscription_index.hpp"

namespace tessera::net {

const uint32_t* SubscriptionIndex::Head(EntityId entity) const noexcept {
    const std::vector<std::unique_ptr<Page>>& pages = types_[entity.type()];
    const size_t page = entity.index() / kPageEntries;
    if (page >= pages.size() || !pages[page]) {
        return nullptr;
    }
    return &(*pages[page])[entity.index() % kPageEntries];
}

uint32_t* SubscriptionIndex::Head(EntityId entity, bool create) {
    std::vector<std::unique_ptr<Page>>& pages = types_[entity.type()];
    const size_t page = entity.index() / kPageEntries;
    if (page >= pages.size()) {
        if (!create) {
            return nullptr;
        }
        pages.resize(page + 1);
    }
    if (!pages[page]) {
        if (!create) {
            return nullptr;
        }
        pages[page] = std::make_unique<Page>(kPageEntries, kNone);
    }
    return &(*pages[page])[entity.index() % kPageEntries];
}

void SubscriptionIndex::Add(EntityId entity, uint32_t connection, uint32_t compact_id) {
    uint32_t* head = Head(entity, true);
    uint32_t at;
    if (free_ != kNone) {
        at = free_;
        free_ = nodes_[at].next;
    } else {
        at = static_cast<uint32_t>(nodes_.size());
        nodes_.push_back(Node{});
    }
    nodes_[at] = Node{compact_id, static_cast<uint16_t>(connection),
                      static_cast<uint16_t>(entity.generation()), *head};
    *head = at;
    ++count_;
}

bool SubscriptionIndex::Remove(EntityId entity, uint32_t connection) {
    uint32_t* head = Head(entity, false);
    if (head == nullptr) {
        return false;
    }
    uint32_t* link = head; // the place that holds the index of the node being looked at
    while (*link != kNone) {
        Node& node = nodes_[*link];
        if (node.connection == connection && node.generation == entity.generation()) {
            const uint32_t removed = *link;
            *link = node.next;
            node.next = free_;
            free_ = removed;
            --count_;
            return true;
        }
        link = &node.next;
    }
    return false;
}

size_t SubscriptionIndex::MemoryBytes() const noexcept {
    size_t bytes = nodes_.capacity() * sizeof(Node);
    for (const std::vector<std::unique_ptr<Page>>& pages : types_) {
        bytes += pages.capacity() * sizeof(std::unique_ptr<Page>);
        for (const std::unique_ptr<Page>& page : pages) {
            if (page) {
                bytes += page->capacity() * sizeof(uint32_t);
            }
        }
    }
    return bytes;
}

} // namespace tessera::net
