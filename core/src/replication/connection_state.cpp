// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/replication/connection_state.hpp"

#include "tessera/net/replication/record_codec.hpp"
#include "tessera/net/replication/state_store.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace tessera::net {

namespace {

constexpr uint64_t kNoEntity = ~0ull; // EntityId::Null().raw()

bool TestBit(const uint64_t* words, uint32_t index) noexcept {
    return ((words[index >> 6] >> (index & 63)) & 1ull) != 0;
}
void SetBit(uint64_t* words, uint32_t index) noexcept {
    words[index >> 6] |= 1ull << (index & 63);
}
void ClearBit(uint64_t* words, uint32_t index) noexcept {
    words[index >> 6] &= ~(1ull << (index & 63));
}

} // namespace

ConnectionState::ConnectionState(const ReplicationSchema& schema, uint32_t max_visible,
                                 uint32_t max_baseline_age_ticks)
    : schema_(schema), max_visible_(max_visible), max_baseline_age_(max_baseline_age_ticks),
      words_(schema.MaxWords()) {
    pages_.resize((static_cast<size_t>(max_visible) + kPageIds - 1) / kPageIds);
}

ConnectionState::Page* ConnectionState::PageOf(uint32_t compact_id, bool create) {
    std::unique_ptr<Page>& slot = pages_[compact_id / kPageIds];
    if (!slot && create) {
        slot = std::make_unique<Page>();
        std::fill(std::begin(slot->entity), std::end(slot->entity), kNoEntity);
        std::fill(std::begin(slot->sent_tick), std::end(slot->sent_tick), 0);
        std::fill(std::begin(slot->acked_tick), std::end(slot->acked_tick), 0);
        slot->acked.assign(static_cast<size_t>(kPageIds) * words_, 0);
        slot->sent.assign(static_cast<size_t>(kPageIds) * words_, 0);
    }
    return slot.get();
}

const ConnectionState::Page* ConnectionState::PageOf(uint32_t compact_id) const noexcept {
    return compact_id < max_visible_ ? pages_[compact_id / kPageIds].get() : nullptr;
}

bool ConnectionState::ValidEnter(const VisibilityEvent& event) noexcept {
    if (schema_.Find(event.id.type()) == nullptr || event.compact_id >= max_visible_) {
        ++untracked_enters_;
        return false;
    }
    return true;
}

void ConnectionState::Release(uint32_t compact_id) {
    Page* page = PageOf(compact_id, false);
    if (page == nullptr) {
        return;
    }
    const uint32_t local = compact_id % kPageIds;
    page->entity[local] = kNoEntity;
    ClearBit(page->in_use, local);
    ClearBit(page->pending, local);
    ClearBit(page->stale, local);
    ClearBit(page->has_baseline, local);
    ClearBit(page->outstanding, local);
}

void ConnectionState::EnterOne(uint64_t raw, uint32_t compact_id) {
    Page* page = PageOf(compact_id, true);
    const uint32_t local = compact_id % kPageIds;
    page->entity[local] = raw;
    page->sent_tick[local] = 0;
    page->acked_tick[local] = 0;
    SetBit(page->in_use, local);
    SetBit(page->pending, local); // no confirmed state yet, so the entity is sent in full
    SetBit(page->stale, local);
    ClearBit(page->has_baseline, local);
    ClearBit(page->outstanding, local);
}

void ConnectionState::ApplyEvents(std::span<const VisibilityEvent> events) {
    // Departures first: the relevancy engine reports Destroy and Leave before Enter, and an id can be
    // taken over in the same call.
    leaving_.clear();
    for (const VisibilityEvent& event : events) {
        if (event.kind == VisibilityEvent::Kind::Enter) {
            continue;
        }
        const VisibleEntry* entry = visible_.Find(event.id.raw());
        if (entry != nullptr) {
            leaving_.push_back(entry->raw);
            Release(entry->compact_id);
        }
    }
    if (!leaving_.empty()) {
        std::sort(leaving_.begin(), leaving_.end());
        leaving_.erase(std::unique(leaving_.begin(), leaving_.end()), leaving_.end());
        visible_.Apply(leaving_, {});
    }

    // Arrivals. The relevancy engine never reuses a compact id that is in use or reports an entity that is
    // already visible; when a caller does, each arrival is applied on its own so that it replaces the old
    // one.
    entering_.clear();
    bool conflict = false;
    for (const VisibilityEvent& event : events) {
        if (event.kind != VisibilityEvent::Kind::Enter || !ValidEnter(event)) {
            continue;
        }
        const Page* page = PageOf(event.compact_id);
        if ((page != nullptr && TestBit(page->in_use, event.compact_id % kPageIds)) ||
            visible_.Contains(event.id.raw())) {
            conflict = true;
        }
        entering_.push_back(VisibleEntry{event.id.raw(), event.compact_id});
    }
    if (!conflict && entering_.size() > 1) {
        std::vector<uint64_t> raws, ids;
        for (const VisibleEntry& entry : entering_) {
            raws.push_back(entry.raw);
            ids.push_back(entry.compact_id);
        }
        std::sort(raws.begin(), raws.end());
        std::sort(ids.begin(), ids.end());
        conflict = std::adjacent_find(raws.begin(), raws.end()) != raws.end() ||
                   std::adjacent_find(ids.begin(), ids.end()) != ids.end();
    }

    if (!conflict) {
        for (const VisibleEntry& entry : entering_) {
            EnterOne(entry.raw, entry.compact_id);
        }
        std::sort(entering_.begin(), entering_.end(),
                  [](const VisibleEntry& a, const VisibleEntry& b) { return a.raw < b.raw; });
        visible_.Apply({}, entering_);
        return;
    }
    for (const VisibleEntry& entry : entering_) {
        const Page* page = PageOf(entry.compact_id);
        if (page != nullptr && TestBit(page->in_use, entry.compact_id % kPageIds)) {
            const uint64_t old_raw = page->entity[entry.compact_id % kPageIds];
            Release(entry.compact_id);
            const uint64_t gone[] = {old_raw};
            visible_.Apply(gone, {});
        }
        if (const VisibleEntry* again = visible_.Find(entry.raw)) {
            const uint32_t old_id = again->compact_id;
            Release(old_id);
            const uint64_t gone[] = {entry.raw};
            visible_.Apply(gone, {});
        }
        EnterOne(entry.raw, entry.compact_id);
        const VisibleEntry added[] = {entry};
        visible_.Apply({}, added);
    }
}

void ConnectionState::MarkChanged(std::span<const EntityId> changed) {
    for (const EntityId id : changed) {
        const VisibleEntry* entry = visible_.Find(id.raw());
        if (entry == nullptr) {
            continue;
        }
        Page* page = PageOf(entry->compact_id, false);
        const uint32_t local = entry->compact_id % kPageIds;
        SetBit(page->pending, local);
        SetBit(page->stale, local);
    }
}

uint32_t ConnectionState::CompactIdOf(EntityId id) const noexcept {
    const VisibleEntry* entry = visible_.Find(id.raw());
    return entry != nullptr ? entry->compact_id : kInvalidCompactId;
}

EntityId ConnectionState::EntityOf(uint32_t compact_id) const noexcept {
    const Page* page = PageOf(compact_id);
    if (page == nullptr || !TestBit(page->in_use, compact_id % kPageIds)) {
        return EntityId::Null();
    }
    return EntityId::FromRaw(page->entity[compact_id % kPageIds]);
}

bool ConnectionState::IsPending(uint32_t compact_id) const noexcept {
    const Page* page = PageOf(compact_id);
    return page != nullptr && TestBit(page->pending, compact_id % kPageIds);
}

bool ConnectionState::HasBaseline(uint32_t compact_id) const noexcept {
    const Page* page = PageOf(compact_id);
    return page != nullptr && TestBit(page->has_baseline, compact_id % kPageIds);
}

EncodeStats ConnectionState::Encode(uint64_t tick, const StateStore& store, std::vector<uint8_t>& out) {
    out.clear();
    EncodeStats stats;
    BitWriter writer(out);
    int64_t previous = -1;
    for (uint32_t page_index = 0; page_index < pages_.size(); ++page_index) {
        Page* page = pages_[page_index].get();
        if (page == nullptr) {
            continue;
        }
        for (uint32_t word = 0; word < kBitWords; ++word) {
            uint64_t bits = page->pending[word];
            while (bits != 0) {
                const uint32_t local = word * 64 + static_cast<uint32_t>(std::countr_zero(bits));
                bits &= bits - 1;
                const EntityId id = EntityId::FromRaw(page->entity[local]);
                const uint32_t* current = store.Find(id);
                if (current == nullptr) {
                    ++stats.skipped_unquantized; // it stays pending until it has a state
                    continue;
                }
                const TypeSchema& type = *schema_.Find(id.type());
                uint32_t* acked = page->acked.data() + static_cast<size_t>(local) * words_;
                uint32_t* sent = page->sent.data() + static_cast<size_t>(local) * words_;
                const bool has_baseline = TestBit(page->has_baseline, local);
                const bool outstanding = TestBit(page->outstanding, local);
                if (!outstanding && has_baseline &&
                    std::memcmp(acked, current, type.words * sizeof(uint32_t)) == 0) {
                    ClearBit(page->pending, local); // the client already has exactly this
                    ClearBit(page->stale, local);
                    continue;
                }
                const uint64_t age = tick - page->acked_tick[local]; // huge if the tick went backwards
                const bool use_baseline = has_baseline && age >= 1 && age <= max_baseline_age_;
                const uint32_t compact_id = page_index * kPageIds + local;
                WriteRecord(type, static_cast<uint32_t>(static_cast<int64_t>(compact_id) - previous),
                            use_baseline ? static_cast<uint32_t>(age) : 0, use_baseline ? acked : nullptr,
                            current, writer);
                ++stats.records;
                if (!use_baseline) {
                    ++stats.complete_records;
                }
                std::memcpy(sent, current, type.words * sizeof(uint32_t));
                page->sent_tick[local] = tick;
                SetBit(page->outstanding, local);
                ClearBit(page->stale, local);
                previous = compact_id;
            }
        }
    }
    WriteTerminator(writer);
    writer.Finish();
    return stats;
}

void ConnectionState::Confirm(uint64_t tick) {
    for (const std::unique_ptr<Page>& entry : pages_) {
        Page* page = entry.get();
        if (page == nullptr) {
            continue;
        }
        for (uint32_t word = 0; word < kBitWords; ++word) {
            uint64_t bits = page->pending[word] & page->outstanding[word];
            while (bits != 0) {
                const uint32_t local = word * 64 + static_cast<uint32_t>(std::countr_zero(bits));
                bits &= bits - 1;
                if (page->sent_tick[local] != tick) {
                    continue; // sent again since, or sent in another buffer
                }
                const TypeSchema& type = *schema_.Find(EntityId::FromRaw(page->entity[local]).type());
                std::memcpy(page->acked.data() + static_cast<size_t>(local) * words_,
                            page->sent.data() + static_cast<size_t>(local) * words_,
                            type.words * sizeof(uint32_t));
                page->acked_tick[local] = tick;
                SetBit(page->has_baseline, local);
                ClearBit(page->outstanding, local);
                if (!TestBit(page->stale, local)) {
                    ClearBit(page->pending, local);
                }
            }
        }
    }
}

size_t ConnectionState::MemoryBytes() const noexcept {
    size_t bytes =
        pages_.capacity() * sizeof(std::unique_ptr<Page>) + visible_.Entries().size() * sizeof(VisibleEntry);
    for (const std::unique_ptr<Page>& page : pages_) {
        if (page) {
            bytes += sizeof(Page) + (page->acked.capacity() + page->sent.capacity()) * sizeof(uint32_t);
        }
    }
    return bytes;
}

} // namespace tessera::net
