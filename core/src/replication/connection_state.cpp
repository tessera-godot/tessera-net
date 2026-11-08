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
                                 uint32_t max_baseline_age_ticks, uint32_t max_unconfirmed_sends)
    : schema_(schema), max_visible_(max_visible), max_baseline_age_(max_baseline_age_ticks),
      ring_(max_unconfirmed_sends < 1 ? 1 : (max_unconfirmed_sends > 255 ? 255 : max_unconfirmed_sends)),
      words_(schema.MaxWords()) {
    pages_.resize((static_cast<size_t>(max_visible) + kPageIds - 1) / kPageIds);
    for (uint32_t type = 0; type < type_full_bytes_.size(); ++type) {
        if (const TypeSchema* found = schema.Find(type)) {
            // Until a record of the type has been written: room for every word in full and the record head.
            type_full_bytes_[type] = static_cast<uint16_t>(std::min<uint32_t>(found->words * 4 + 4, 65535));
        }
    }
}

ConnectionState::Page* ConnectionState::PageOf(uint32_t compact_id, bool create) {
    std::unique_ptr<Page>& slot = pages_[compact_id / kPageIds];
    if (!slot && create) {
        slot = std::make_unique<Page>();
        std::fill(std::begin(slot->entity), std::end(slot->entity), kNoEntity);
        std::fill(std::begin(slot->acked_tick), std::end(slot->acked_tick), 0);
        slot->acked.assign(static_cast<size_t>(kPageIds) * words_, 0);
        slot->sent.assign(static_cast<size_t>(kPageIds) * ring_ * words_, 0);
        slot->sent_ticks.assign(static_cast<size_t>(kPageIds) * ring_, 0);
        slot->sent_runs.assign(static_cast<size_t>(kPageIds) * ring_, 0);
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
    page->sent_count[local] = 0;
    page->sent_head[local] = 0;
    page->ready_since[local] = 0;
    page->last_bytes[local] = 0;
    ClearBit(page->in_use, local);
    ClearBit(page->pending, local);
    ClearBit(page->stale, local);
    ClearBit(page->has_baseline, local);
    ClearBit(page->outstanding, local);
}

void ConnectionState::EnterOne(uint64_t raw, uint32_t compact_id) {
    changes_.push_back(SubscriptionChange{true, raw, compact_id});
    Page* page = PageOf(compact_id, true);
    const uint32_t local = compact_id % kPageIds;
    page->entity[local] = raw;
    page->sent_count[local] = 0;
    page->sent_head[local] = 0;
    page->ready_since[local] = 0;
    page->last_bytes[local] = 0;
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
    changes_.clear();
    leaving_.clear();
    for (const VisibilityEvent& event : events) {
        if (event.kind == VisibilityEvent::Kind::Enter) {
            continue;
        }
        const VisibleEntry* entry = visible_.Find(event.id.raw());
        if (entry != nullptr) {
            leaving_.push_back(entry->raw);
            changes_.push_back(SubscriptionChange{false, entry->raw, entry->compact_id});
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
            changes_.push_back(SubscriptionChange{false, old_raw, entry.compact_id});
            Release(entry.compact_id);
            const uint64_t gone[] = {old_raw};
            visible_.Apply(gone, {});
        }
        if (const VisibleEntry* again = visible_.Find(entry.raw)) {
            const uint32_t old_id = again->compact_id;
            changes_.push_back(SubscriptionChange{false, entry.raw, old_id});
            Release(old_id);
            const uint64_t gone[] = {entry.raw};
            visible_.Apply(gone, {});
        }
        EnterOne(entry.raw, entry.compact_id);
        const VisibleEntry added[] = {entry};
        visible_.Apply({}, added);
    }
}

void ConnectionState::MarkPendingAt(uint32_t compact_id) noexcept {
    Page* page = compact_id < max_visible_ ? pages_[compact_id / kPageIds].get() : nullptr;
    if (page == nullptr) {
        return;
    }
    const uint32_t local = compact_id % kPageIds;
    if (TestBit(page->in_use, local)) {
        SetBit(page->pending, local);
        SetBit(page->stale, local);
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

uint32_t ConnectionState::LastRecordBytes(uint32_t compact_id) const noexcept {
    const Page* page = PageOf(compact_id);
    return page != nullptr ? page->last_bytes[compact_id % kPageIds] : 0;
}

bool ConnectionState::HasBaseline(uint32_t compact_id) const noexcept {
    const Page* page = PageOf(compact_id);
    return page != nullptr && TestBit(page->has_baseline, compact_id % kPageIds);
}

namespace {

constexpr uint32_t kTerminatorBits = 4; // the end of a buffer is a gap of 0: one chunk of a variable number

} // namespace

bool ConnectionState::WriteEntity(Page& page, uint32_t page_index, uint32_t local, uint64_t tick,
                                  const uint32_t* current, uint32_t max_bytes, BitWriter& writer,
                                  int64_t& previous, EncodeStats& stats) {
    const EntityId id = EntityId::FromRaw(page.entity[local]);
    const TypeSchema& type = *schema_.Find(id.type());
    uint32_t* acked = page.acked.data() + static_cast<size_t>(local) * words_;
    const bool has_baseline = TestBit(page.has_baseline, local);
    const uint64_t age = tick - page.acked_tick[local]; // huge if the tick went backwards
    const bool use_baseline = has_baseline && age >= 1 && age <= max_baseline_age_;
    const uint32_t compact_id = page_index * kPageIds + local;
    const BitWriter::Mark mark = writer.GetMark();
    const uint64_t bits_before = writer.BitCount();
    WriteRecord(type, static_cast<uint32_t>(static_cast<int64_t>(compact_id) - previous),
                use_baseline ? static_cast<uint32_t>(age) : 0, use_baseline ? acked : nullptr, current,
                writer);
    const uint64_t bits = writer.BitCount() - bits_before;
    if (max_bytes > 0 && (writer.BitCount() + kTerminatorBits + 7) / 8 > max_bytes) {
        writer.Rewind(mark); // it does not fit: nothing changes but the estimate, which now is the real size
        page.last_bytes[local] = static_cast<uint16_t>(std::min<uint64_t>((bits + 7) / 8, 65535));
        return false;
    }
    ++stats.records;
    if (!use_baseline) {
        ++stats.complete_records;
    }
    const uint16_t bytes = static_cast<uint16_t>(std::min<uint64_t>((bits + 7) / 8, 65535));
    page.last_bytes[local] = bytes;
    page.ready_since[local] = 0;
    if (!use_baseline) { // follow what a complete record of this type takes
        uint16_t& estimate = type_full_bytes_[id.type()];
        estimate = static_cast<uint16_t>((3u * estimate + bytes + 2) / 4);
    }
    // Remember this send, so that its confirmation can be used even if the entity is sent again
    // first. Sending the very same state on the very next tick extends the newest remembered send
    // instead of taking a new place, so an entity that does not change never runs out of room.
    const size_t ring_base = static_cast<size_t>(local) * ring_;
    bool extended = false;
    if (page.sent_count[local] > 0) {
        const uint32_t newest = (page.sent_head[local] + ring_ - 1) % ring_;
        if (page.sent_ticks[ring_base + newest] + 1 == tick &&
            std::memcmp(page.sent.data() + (ring_base + newest) * words_, current,
                        type.words * sizeof(uint32_t)) == 0) {
            page.sent_ticks[ring_base + newest] = tick;
            ++page.sent_runs[ring_base + newest];
            extended = true;
        }
    }
    if (!extended) {
        const uint32_t slot = page.sent_head[local];
        std::memcpy(page.sent.data() + (ring_base + slot) * words_, current, type.words * sizeof(uint32_t));
        page.sent_ticks[ring_base + slot] = tick;
        page.sent_runs[ring_base + slot] = 1;
        page.sent_head[local] = static_cast<uint8_t>((slot + 1) % ring_);
        if (page.sent_count[local] < ring_) {
            ++page.sent_count[local];
        }
    }
    SetBit(page.outstanding, local);
    ClearBit(page.stale, local);
    previous = compact_id;
    return true;
}

EncodeStats ConnectionState::Encode(uint64_t tick, const StateStore& store, std::vector<uint8_t>& out,
                                    const EncodeLimits& limits) {
    out.clear();
    EncodeStats stats;
    BitWriter writer(out);
    int64_t previous = -1;
    const bool limited = limits.max_bytes > 0;
    candidates_.clear();
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
                const uint32_t* acked = page->acked.data() + static_cast<size_t>(local) * words_;
                const bool has_baseline = TestBit(page->has_baseline, local);
                const bool outstanding = TestBit(page->outstanding, local);
                if (!outstanding && has_baseline &&
                    std::memcmp(acked, current, type.words * sizeof(uint32_t)) == 0) {
                    ClearBit(page->pending, local); // the client already has exactly this
                    ClearBit(page->stale, local);
                    page->ready_since[local] = 0; // it waits for nothing: an old wait must not come back
                    continue;
                }
                // Sent, not confirmed and not changed since: the send may still be on its way, so wait for
                // it.
                if (!TestBit(page->stale, local) && outstanding && page->sent_count[local] > 0) {
                    const uint32_t newest = (page->sent_head[local] + ring_ - 1) % ring_;
                    const uint64_t last_sent = page->sent_ticks[static_cast<size_t>(local) * ring_ + newest];
                    if (tick - last_sent < limits.resend_after_ticks) { // huge if the tick went backwards
                        ++stats.waiting;
                        continue;
                    }
                }
                if (!limited) {
                    WriteEntity(*page, page_index, local, tick, current, 0, writer, previous, stats);
                    continue;
                }
                if (page->ready_since[local] == 0) {
                    page->ready_since[local] = tick + 1;
                }
                const uint32_t estimate =
                    page->last_bytes[local] != 0 ? page->last_bytes[local] : type_full_bytes_[id.type()];
                candidates_.push_back(Candidate{page_index * kPageIds + local, estimate, 0});
            }
        }
    }
    if (limited) {
        // Room in the buffer for records: all of it but the end marker.
        const uint32_t room = limits.max_bytes - 1;
        uint64_t wanted = 0;
        for (const Candidate& candidate : candidates_) {
            wanted += candidate.estimated_bytes;
        }
        chosen_.clear();
        if (wanted <= room) {
            for (const Candidate& candidate : candidates_) { // everything fits: in id order, as it was found
                chosen_.push_back(candidate.compact_id);
            }
        } else {
            // Not everything fits: the longest wait, times the weight of the type, goes first. Ties go to the
            // lowest id, so the choice never depends on anything but the state.
            for (Candidate& candidate : candidates_) {
                const Page& page = *pages_[candidate.compact_id / kPageIds];
                const uint32_t local = candidate.compact_id % kPageIds;
                const uint64_t waited = tick + 1 - page.ready_since[local];
                const uint32_t weight = limits.type_weight != nullptr
                                            ? limits.type_weight[EntityId::FromRaw(page.entity[local]).type()]
                                            : 1;
                candidate.score = (waited + 1) * weight;
            }
            std::sort(candidates_.begin(), candidates_.end(), [](const Candidate& a, const Candidate& b) {
                return a.score != b.score ? a.score > b.score : a.compact_id < b.compact_id;
            });
            uint32_t left = room;
            for (const Candidate& candidate : candidates_) {
                if (candidate.estimated_bytes <= left) {
                    left -= candidate.estimated_bytes;
                    chosen_.push_back(candidate.compact_id);
                }
            }
            std::sort(chosen_.begin(), chosen_.end()); // the format wants ascending ids
        }
        uint32_t written = 0;
        for (const uint32_t compact_id : chosen_) {
            Page& page = *pages_[compact_id / kPageIds];
            const uint32_t local = compact_id % kPageIds;
            const uint32_t* current = store.Find(EntityId::FromRaw(page.entity[local]));
            if (WriteEntity(page, compact_id / kPageIds, local, tick, current, limits.max_bytes, writer,
                            previous, stats)) {
                ++written;
            }
        }
        stats.deferred = static_cast<uint32_t>(candidates_.size()) - written;
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
                // Find the send made at this tick, newest first. Everything sent after it stays outstanding.
                const uint32_t count = page->sent_count[local];
                uint32_t newer = 0;
                uint32_t found = ring_;
                for (; newer < count; ++newer) {
                    const uint32_t slot = (page->sent_head[local] + ring_ - 1 - newer) % ring_;
                    // A remembered send covers the ticks from `last - runs + 1` to `last`: the same state,
                    // sent on every one of them.
                    const size_t at = static_cast<size_t>(local) * ring_ + slot;
                    const uint64_t last = page->sent_ticks[at];
                    if (tick <= last && tick + page->sent_runs[at] > last) {
                        found = slot;
                        break;
                    }
                }
                if (found == ring_) {
                    continue; // not sent at that tick, or forgotten since
                }
                const TypeSchema& type = *schema_.Find(EntityId::FromRaw(page->entity[local]).type());
                std::memcpy(page->acked.data() + static_cast<size_t>(local) * words_,
                            page->sent.data() + (static_cast<size_t>(local) * ring_ + found) * words_,
                            type.words * sizeof(uint32_t));
                page->acked_tick[local] = tick;
                SetBit(page->has_baseline, local);
                uint32_t still_outstanding = newer; // the confirmed send and the older ones are done
                if (newer > 0 && !TestBit(page->stale, local)) {
                    // Every later send carried the very state the client has now confirmed, and nothing
                    // changed since: they only repeated it, so the client is up to date. Every one of them
                    // has to match, not only the newest: the client may hold a state in between, such as B in
                    // A, B, A.
                    bool all_same = true;
                    for (uint32_t later = 0; later < newer && all_same; ++later) {
                        const uint32_t slot = (page->sent_head[local] + ring_ - 1 - later) % ring_;
                        all_same = std::memcmp(page->acked.data() + static_cast<size_t>(local) * words_,
                                               page->sent.data() +
                                                   (static_cast<size_t>(local) * ring_ + slot) * words_,
                                               type.words * sizeof(uint32_t)) == 0;
                    }
                    if (all_same) {
                        still_outstanding = 0;
                    }
                }
                page->sent_count[local] = static_cast<uint8_t>(still_outstanding);
                if (still_outstanding == 0) {
                    ClearBit(page->outstanding, local);
                    if (!TestBit(page->stale, local)) {
                        ClearBit(page->pending, local);
                        page->ready_since[local] = 0; // it waits for nothing: an old wait must not come back
                    }
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
            bytes += sizeof(Page) + (page->acked.capacity() + page->sent.capacity()) * sizeof(uint32_t) +
                     page->sent_ticks.capacity() * sizeof(uint64_t) +
                     page->sent_runs.capacity() * sizeof(uint32_t);
        }
    }
    return bytes;
}

} // namespace tessera::net
