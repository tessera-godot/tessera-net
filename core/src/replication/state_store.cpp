// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/replication/state_store.hpp"

namespace tessera::net {

StateStore::TypeStore& StateStore::StoreFor(uint32_t type, size_t pages_needed) {
    std::unique_ptr<TypeStore>& slot = stores_[type];
    if (!slot) {
        slot = std::make_unique<TypeStore>();
        slot->schema = schema_.Find(type);
    }
    TypeStore& store = *slot;
    while (store.pages.size() < pages_needed) {
        auto page = std::make_unique<Page>();
        page->words.assign(static_cast<size_t>(kPageSlots) * store.schema->words, 0);
        page->generation.assign(kPageSlots, kNoGeneration);
        store.pages.push_back(std::move(page));
    }
    return store;
}

const uint32_t* StateStore::Find(EntityId id) const noexcept {
    if (id.type() >= stores_.size() || !stores_[id.type()]) {
        return nullptr;
    }
    const TypeStore& store = *stores_[id.type()];
    const uint32_t page_index = id.index() / kPageSlots;
    if (page_index >= store.pages.size()) {
        return nullptr;
    }
    const Page& page = *store.pages[page_index];
    const uint32_t slot = id.index() % kPageSlots;
    if (page.generation[slot] != id.generation()) {
        return nullptr;
    }
    return page.words.data() + static_cast<size_t>(slot) * store.schema->words;
}

size_t StateStore::MemoryBytes() const noexcept {
    size_t bytes = changed_.capacity() * sizeof(EntityId);
    for (const std::unique_ptr<TypeStore>& store : stores_) {
        if (!store) {
            continue;
        }
        for (const std::unique_ptr<Page>& page : store->pages) {
            bytes +=
                page->words.capacity() * sizeof(uint32_t) + page->generation.capacity() * sizeof(uint32_t);
        }
    }
    for (const std::vector<uint32_t>& changes : page_changes_) {
        bytes += changes.capacity() * sizeof(uint32_t);
    }
    return bytes;
}

} // namespace tessera::net
