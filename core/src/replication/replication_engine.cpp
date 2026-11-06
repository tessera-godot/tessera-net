// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/replication/replication_engine.hpp"

#include <stdexcept>

namespace tessera::net {

ReplicationEngine::ReplicationEngine(const ReplicationSchema& schema, const ReplicationConfig& config,
                                     IJobSystem& jobs)
    : config_(config), schema_(schema), jobs_(jobs), store_(schema) {
    if (config.max_connections == 0) {
        throw std::invalid_argument("ReplicationConfig::max_connections must be at least 1");
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
    connections_[connection].reset();
    return true;
}

void ReplicationEngine::ApplyVisibility(ConnectionId connection, std::span<const VisibilityEvent> events) {
    if (connection < connections_.size() && connections_[connection]) {
        connections_[connection]->state.ApplyEvents(events);
    }
}

void ReplicationEngine::Update(uint64_t tick) {
    active_.clear();
    for (uint32_t id = 0; id < connections_.size(); ++id) {
        if (connections_[id]) {
            active_.push_back(id);
        }
    }
    const std::span<const EntityId> changed = store_.Changed();
    // Every connection reads the shared store and the list of changes and writes only its own state and
    // buffer, so they can all run at once.
    jobs_.ParallelFor(static_cast<uint32_t>(active_.size()), 1, [&](uint32_t begin, uint32_t end, uint32_t) {
        for (uint32_t i = begin; i < end; ++i) {
            Entry& entry = *connections_[active_[i]];
            entry.state.MarkChanged(changed);
            entry.stats = entry.state.Encode(tick, store_, entry.output);
        }
    });
    last_stats_ = EncodeStats{};
    for (const uint32_t id : active_) { // added up in connection order, whatever the workers did
        const EncodeStats& stats = connections_[id]->stats;
        last_stats_.records += stats.records;
        last_stats_.complete_records += stats.complete_records;
        last_stats_.skipped_unquantized += stats.skipped_unquantized;
    }
    store_.ClearChanged();
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
