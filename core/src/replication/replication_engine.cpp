// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/replication/replication_engine.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace tessera::net {

namespace {

// What a state buffer costs beyond its bytes: the message header (the tick and the number of event batches)
// and the room a packet takes.
constexpr uint32_t kBufferOverheadBytes = 12;

} // namespace

ReplicationEngine::ReplicationEngine(const ReplicationSchema& schema, const ReplicationConfig& config,
                                     IJobSystem& jobs)
    : config_(config), schema_(schema), jobs_(jobs), store_(schema) {
    if (config.max_connections == 0) {
        throw std::invalid_argument("ReplicationConfig::max_connections must be at least 1");
    }
    if (config.max_connections > 65535) {
        throw std::invalid_argument("ReplicationConfig::max_connections must be at most 65535");
    }
    if (config.max_visible_per_connection == 0) {
        throw std::invalid_argument("ReplicationConfig::max_visible_per_connection must be at least 1");
    }
    if (config.max_baseline_age_ticks == 0) {
        throw std::invalid_argument("ReplicationConfig::max_baseline_age_ticks must be at least 1");
    }
    if (config.max_unconfirmed_sends == 0 || config.max_unconfirmed_sends > 255) {
        throw std::invalid_argument("ReplicationConfig::max_unconfirmed_sends must be 1 to 255");
    }
    if (config.tick_rate == 0 || config.tick_rate > 1000) {
        throw std::invalid_argument("ReplicationConfig::tick_rate must be 1 to 1000");
    }
    if (config.rate_kbps > 1'000'000) {
        throw std::invalid_argument("ReplicationConfig::rate_kbps must be at most 1000000");
    }
    if (config.peak_kbps > 1'000'000 || (config.peak_kbps != 0 && config.peak_kbps < config.rate_kbps)) {
        throw std::invalid_argument("ReplicationConfig::peak_kbps must be 0, or from rate_kbps to 1000000");
    }
    if (config.rate_kbps > 0) {
        if (config.max_buffer_bytes < 64 || config.max_buffer_bytes > 65535) {
            throw std::invalid_argument("ReplicationConfig::max_buffer_bytes must be 64 to 65535");
        }
        // A record that cannot fit in an empty buffer would never be sent.
        const uint32_t largest_record = 4 * schema.MaxWords() + 16;
        if (config.max_buffer_bytes < largest_record) {
            throw std::invalid_argument("ReplicationConfig::max_buffer_bytes must hold the largest record (" +
                                        std::to_string(largest_record) + " bytes for this schema)");
        }
        // A record larger than what one tick of the rate carries (with the headers of the buffer) would
        // never be sent: under load the bucket holds about one tick.
        const uint64_t bytes_per_tick = static_cast<uint64_t>(config.rate_kbps) * 125 / config.tick_rate;
        if (bytes_per_tick < largest_record + kBufferOverheadBytes) {
            throw std::invalid_argument(
                "ReplicationConfig::rate_kbps must carry the largest record in one tick (" +
                std::to_string(largest_record + kBufferOverheadBytes) +
                " bytes for this schema, with the headers)");
        }
    }
    if (config.resend_margin_ticks > 30) {
        throw std::invalid_argument("ReplicationConfig::resend_margin_ticks must be 0 to 30");
    }
    if (config.min_resend_ticks == 0 || config.min_resend_ticks > 255) {
        throw std::invalid_argument("ReplicationConfig::min_resend_ticks must be 1 to 255");
    }
    if (config.max_resend_ticks < config.min_resend_ticks || config.max_resend_ticks > 255) {
        throw std::invalid_argument(
            "ReplicationConfig::max_resend_ticks must be from min_resend_ticks to 255");
    }
    if (config.initial_resend_ticks < config.min_resend_ticks ||
        config.initial_resend_ticks > config.max_resend_ticks) {
        throw std::invalid_argument(
            "ReplicationConfig::initial_resend_ticks must be from min_resend_ticks to max_resend_ticks");
    }
    for (const uint8_t weight : config.type_weight) {
        if (weight == 0) {
            throw std::invalid_argument("ReplicationConfig::type_weight must be 1 to 255 for every type");
        }
    }
    connections_.resize(config.max_connections);
}

bool ReplicationEngine::AddConnection(ConnectionId connection) {
    if (connection >= connections_.size() || connections_[connection]) {
        return false;
    }
    connections_[connection] = std::make_unique<Entry>(schema_, config_);
    return true;
}

bool ReplicationEngine::RemoveConnection(ConnectionId connection) {
    if (connection >= connections_.size() || !connections_[connection]) {
        return false;
    }
    for (const VisibleEntry& entry : connections_[connection]->state.Visible()) {
        subscriptions_.Remove(EntityId::FromRaw(entry.raw), connection);
    }
    connections_[connection].reset();
    return true;
}

void ReplicationEngine::ApplyVisibility(ConnectionId connection, std::span<const VisibilityEvent> events) {
    if (connection < connections_.size() && connections_[connection]) {
        ConnectionState& state = connections_[connection]->state;
        state.ApplyEvents(events);
        for (const SubscriptionChange& change : state.LastChanges()) {
            if (change.added) {
                subscriptions_.Add(EntityId::FromRaw(change.raw), connection, change.compact_id);
            } else {
                subscriptions_.Remove(EntityId::FromRaw(change.raw), connection);
            }
        }
    }
}

void ReplicationEngine::Update(uint64_t tick) {
    active_.clear();
    for (uint32_t id = 0; id < connections_.size(); ++id) {
        if (connections_[id]) {
            active_.push_back(id);
        }
    }
    // Mark what changed: each entity reaches only the connections that see it, whatever their number.
    for (const EntityId id : store_.Changed()) {
        subscriptions_.ForEach(id, [&](uint32_t connection, uint32_t compact_id) {
            connections_[connection]->state.MarkPendingAt(compact_id);
        });
    }
    // Every connection reads the shared store and writes only its own state and buffer, so they can all run
    // at once.
    jobs_.ParallelFor(static_cast<uint32_t>(active_.size()), 1, [&](uint32_t begin, uint32_t end, uint32_t) {
        for (uint32_t i = begin; i < end; ++i) {
            Entry& entry = *connections_[active_[i]];
            entry.bucket.Refill();
            EncodeLimits limits;
            limits.resend_after_ticks = entry.rtt.ResendAfter();
            limits.type_weight = config_.type_weight.data();
            if (!entry.bucket.Unlimited()) {
                limits.max_bytes = static_cast<uint32_t>(
                    std::clamp<int64_t>(entry.bucket.AvailableBytes(), 1, config_.max_buffer_bytes));
            }
            entry.stats = entry.state.Encode(tick, store_, entry.output, limits);
            if (!entry.bucket.Unlimited() &&
                entry.output.size() > 1) { // more than the end marker: it is sent
                entry.bucket.Charge(static_cast<uint32_t>(entry.output.size()) + kBufferOverheadBytes);
            }
        }
    });
    last_stats_ = EncodeStats{};
    for (const uint32_t id : active_) { // added up in connection order, whatever the workers did
        const EncodeStats& stats = connections_[id]->stats;
        last_stats_.records += stats.records;
        last_stats_.complete_records += stats.complete_records;
        last_stats_.skipped_unquantized += stats.skipped_unquantized;
        last_stats_.waiting += stats.waiting;
        last_stats_.deferred += stats.deferred;
    }
    store_.ClearChanged();
}

void ReplicationEngine::NoteRoundTrip(ConnectionId connection, uint32_t ticks) {
    if (connection < connections_.size() && connections_[connection]) {
        connections_[connection]->rtt.Note(ticks);
    }
}

void ReplicationEngine::Charge(ConnectionId connection, uint32_t bytes) {
    if (connection < connections_.size() && connections_[connection]) {
        connections_[connection]->bucket.Charge(bytes);
    }
}

uint32_t ReplicationEngine::ResendAfterTicks(ConnectionId connection) const noexcept {
    if (connection >= connections_.size() || !connections_[connection]) {
        return 0;
    }
    return connections_[connection]->rtt.ResendAfter();
}

int64_t ReplicationEngine::BudgetBytes(ConnectionId connection) const noexcept {
    if (connection >= connections_.size() || !connections_[connection]) {
        return 0;
    }
    return connections_[connection]->bucket.AvailableBytes();
}

std::span<const uint8_t> ReplicationEngine::Output(ConnectionId connection) const noexcept {
    if (connection >= connections_.size() || !connections_[connection]) {
        return {};
    }
    return connections_[connection]->output;
}

void ReplicationEngine::Confirm(ConnectionId connection, uint64_t tick) {
    if (connection < connections_.size() && connections_[connection]) {
        connections_[connection]->state.Confirm(tick);
    }
}

const ConnectionState* ReplicationEngine::Connection(ConnectionId connection) const noexcept {
    if (connection >= connections_.size() || !connections_[connection]) {
        return nullptr;
    }
    return &connections_[connection]->state;
}

} // namespace tessera::net
