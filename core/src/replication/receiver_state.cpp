// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/replication/receiver_state.hpp"

#include "tessera/net/replication/record_codec.hpp"

#include <algorithm>
#include <cstring>

namespace tessera::net {

ReceiverState::ReceiverState(const ReplicationSchema& schema, uint32_t max_visible, uint32_t history_ticks)
    : schema_(schema), history_ticks_(history_ticks), slots_(max_visible) {}

bool ReceiverState::OnEnter(uint32_t compact_id, EntityId entity, uint64_t tick) {
    const TypeSchema* type = schema_.Find(entity.type());
    if (compact_id >= slots_.size() || type == nullptr) {
        return false;
    }
    Slot& slot = slots_[compact_id];
    slot.in_use = true;
    slot.entered_tick = tick;
    slot.type = type;
    slot.history.clear();
    return true;
}

void ReceiverState::OnLeave(uint32_t compact_id) {
    if (compact_id < slots_.size()) {
        slots_[compact_id] = Slot{};
    }
}

DecodeStatus ReceiverState::Apply(std::span<const uint8_t> buffer, uint64_t tick, size_t* applied,
                                  IRecordObserver* observer) {
    if (tick > events_tick_) {
        if (applied != nullptr) {
            *applied = 0;
        }
        return DecodeStatus::EventsMissing;
    }
    BitReader reader(buffer);
    int64_t previous = -1;
    size_t count = 0;
    DecodeStatus status = DecodeStatus::Ok;
    while (true) {
        uint32_t gap = 0, age = 0;
        bool complete = false;
        if (!ReadRecordHead(reader, &gap, &complete, &age)) {
            status = reader.ok() ? DecodeStatus::BadRecord : DecodeStatus::Truncated;
            break;
        }
        if (gap == 0) {
            break; // the terminator
        }
        const int64_t id = previous + gap;
        if (id >= static_cast<int64_t>(slots_.size())) {
            status = DecodeStatus::BadRecord;
            break;
        }
        Slot& slot = slots_[static_cast<size_t>(id)];
        if (!slot.in_use) {
            status = DecodeStatus::UnknownEntity;
            break;
        }
        if (slot.entered_tick > tick) {
            status = DecodeStatus::StaleRecord;
            break;
        }
        const Entry* base = nullptr;
        if (!complete) {
            if (age > tick) {
                status = DecodeStatus::MissingBaseline;
                break;
            }
            const uint64_t base_tick = tick - age;
            for (const Entry& entry : slot.history) {
                if (entry.tick == base_tick) {
                    base = &entry;
                    break;
                }
            }
            if (base == nullptr) {
                status = DecodeStatus::MissingBaseline;
                break;
            }
        }
        Entry fresh;
        fresh.tick = tick;
        fresh.words.fill(0);
        if (!ReadRecordBody(*slot.type, reader, complete, base != nullptr ? base->words.data() : nullptr,
                            fresh.words.data())) {
            status = reader.ok() ? DecodeStatus::BadRecord : DecodeStatus::Truncated;
            break;
        }
        // Keep the history ascending by tick; a repeat of a tick replaces the earlier copy.
        auto position = std::lower_bound(slot.history.begin(), slot.history.end(), tick,
                                         [](const Entry& entry, uint64_t t) { return entry.tick < t; });
        if (position != slot.history.end() && position->tick == tick) {
            *position = fresh;
        } else {
            slot.history.insert(position, fresh);
        }
        if (observer != nullptr) {
            observer->OnRecord(static_cast<uint32_t>(id), tick, fresh.words.data());
        }
        const uint64_t newest = slot.history.back().tick;
        while (!slot.history.empty() && slot.history.front().tick + history_ticks_ < newest) {
            slot.history.erase(slot.history.begin());
        }
        ++count;
        previous = id;
    }
    if (applied != nullptr) {
        *applied = count;
    }
    return status;
}

const uint32_t* ReceiverState::Current(uint32_t compact_id) const noexcept {
    if (compact_id >= slots_.size() || slots_[compact_id].history.empty()) {
        return nullptr;
    }
    return slots_[compact_id].history.back().words.data();
}

uint64_t ReceiverState::CurrentTick(uint32_t compact_id) const noexcept {
    if (compact_id >= slots_.size() || slots_[compact_id].history.empty()) {
        return 0;
    }
    return slots_[compact_id].history.back().tick;
}

size_t ReceiverState::HistorySize(uint32_t compact_id) const noexcept {
    return compact_id < slots_.size() ? slots_[compact_id].history.size() : 0;
}

} // namespace tessera::net
