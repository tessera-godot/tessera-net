// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/relevancy/relevancy_engine.hpp"

#include "tessera/net/relevancy/compact_ids.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::net {

namespace {

constexpr ConnectionId kNoOwner = 0xFFFFFFFFu;

const ConnectionEvents kNoEvents{};

void SortUnique(std::vector<uint64_t>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

} // namespace

/// Everything the engine remembers about one connection. A connection's work touches only its own
/// state, which is why connections can be recalculated in parallel without locks.
struct RelevancyEngine::Connection {
    struct Visible {
        uint32_t compact_id;
        uint64_t epoch; // the recalculation that last saw it wanted
    };

    explicit Connection(uint32_t id_capacity) : ids(id_capacity) {}

    CompactIdAllocator ids;
    std::unordered_map<uint64_t, Visible> visible; // EntityId::raw() -> state
    ConnectionEvents events;
    std::vector<Observer> observers;
    std::vector<uint64_t> owned; // owner-only entities this connection owns

    // Scratch vectors reused every recalculation: once they have grown they stop allocating. The `visible`
    // map above still allocates a node for each entity that enters, which the incremental version should
    // avoid (docs/adr/0007-relevancy-first-version.md).
    std::vector<uint64_t> wanted;
    std::vector<EntityId> included;
    std::vector<uint64_t> entering;
    std::vector<uint64_t> leaving;

    uint64_t epoch = 0;
    uint64_t overflow = 0;
};

RelevancyEngine::RelevancyEngine(const RelevancyConfig& config, const SpatialGrid& grid,
                                 RelevancyGroups& groups, IJobSystem& jobs)
    : config_(config), grid_(grid), groups_(groups), jobs_(jobs) {
    ValidateRelevancyConfig(config_);
    connections_.resize(config_.max_connections);
    for (const TypeRelevancy& rule : config_.types) {
        if (rule.mode == RelevancyMode::Spatial && rule.range > 0.0f) {
            query_radius_ = std::max(query_radius_, rule.range + rule.hysteresis_margin);
        }
    }
}

RelevancyEngine::~RelevancyEngine() = default;

bool RelevancyEngine::AddConnection(ConnectionId connection) {
    if (connection >= config_.max_connections || connections_[connection]) {
        return false;
    }
    connections_[connection] = std::make_unique<Connection>(config_.max_visible_per_connection);
    active_.insert(std::lower_bound(active_.begin(), active_.end(), connection), connection);
    return true;
}

bool RelevancyEngine::RemoveConnection(ConnectionId connection) {
    if (connection >= config_.max_connections || !connections_[connection]) {
        return false;
    }
    for (const uint64_t raw : connections_[connection]->owned) {
        owners_[raw] = kNoOwner;
    }
    connections_[connection].reset();
    groups_.RemoveConnection(connection);
    active_.erase(std::lower_bound(active_.begin(), active_.end(), connection));
    return true;
}

bool RelevancyEngine::SetObservers(ConnectionId connection, std::span<const Observer> observers) {
    if (connection >= config_.max_connections || !connections_[connection] ||
        observers.size() > config_.max_observers_per_connection) {
        return false;
    }
    for (const Observer& observer : observers) {
        if (!std::isfinite(observer.x) || !std::isfinite(observer.y) || !std::isfinite(observer.z)) {
            return false;
        }
    }
    connections_[connection]->observers.assign(observers.begin(), observers.end());
    return true;
}

void RelevancyEngine::OnEntityCreated(EntityId entity) {
    switch (config_.types[entity.type()].mode) {
    case RelevancyMode::AlwaysRelevant: {
        const auto it = std::lower_bound(always_.begin(), always_.end(), entity.raw());
        if (it == always_.end() || *it != entity.raw()) {
            always_.insert(it, entity.raw());
        }
        break;
    }
    case RelevancyMode::OwnerOnly:
        owners_.emplace(entity.raw(), kNoOwner);
        break;
    case RelevancyMode::Spatial:
        break; // Found through the grid.
    }
}

void RelevancyEngine::OnEntityDestroyed(EntityId entity) {
    const auto always = std::lower_bound(always_.begin(), always_.end(), entity.raw());
    if (always != always_.end() && *always == entity.raw()) {
        always_.erase(always);
    }
    const auto owner = owners_.find(entity.raw());
    if (owner != owners_.end()) {
        if (owner->second != kNoOwner) {
            std::vector<uint64_t>& owned = connections_[owner->second]->owned;
            owned.erase(std::remove(owned.begin(), owned.end(), entity.raw()), owned.end());
        }
        owners_.erase(owner);
    }
    groups_.RemoveEntity(entity);
    destroyed_.push_back(entity.raw()); // the Destroy events are written by the next Update
}

bool RelevancyEngine::SetOwner(EntityId entity, ConnectionId owner) {
    const auto it = owners_.find(entity.raw());
    if (it == owners_.end() || owner >= config_.max_connections || !connections_[owner]) {
        return false;
    }
    if (it->second != kNoOwner) {
        std::vector<uint64_t>& previous = connections_[it->second]->owned;
        previous.erase(std::remove(previous.begin(), previous.end(), entity.raw()), previous.end());
    }
    it->second = owner;
    connections_[owner]->owned.push_back(entity.raw());
    return true;
}

void RelevancyEngine::Update(uint64_t tick, uint32_t* connections_processed) {
    const uint32_t interval = config_.recalc_interval_ticks;
    const uint64_t phase = tick % interval;
    uint32_t due = 0;
    for (const ConnectionId id : active_) {
        if (id % interval == phase) {
            ++due;
        }
    }
    if (connections_processed != nullptr) {
        *connections_processed = due;
    }
    SortUnique(destroyed_);
    if (!active_.empty()) {
        jobs_.ParallelFor(static_cast<uint32_t>(active_.size()), 1,
                          [&](uint32_t begin, uint32_t end, uint32_t) {
                              for (uint32_t i = begin; i < end; ++i) {
                                  const ConnectionId id = active_[i];
                                  Connection& connection = *connections_[id];
                                  connection.ids.BeginTick();
                                  ApplyDestructions(connection);
                                  if (id % interval == phase) {
                                      Recalculate(id, connection);
                                  }
                              }
                          });
    }
    destroyed_.clear();
}

// Destructions are reported at once, whatever the connection's recalculation schedule. The id stays
// reserved until the next tick, like any other id that is given up.
void RelevancyEngine::ApplyDestructions(Connection& connection) const {
    for (const uint64_t raw : destroyed_) {
        const auto it = connection.visible.find(raw);
        if (it == connection.visible.end()) {
            continue;
        }
        connection.events.events.push_back(
            {VisibilityEvent::Kind::Destroy, EntityId::FromRaw(raw), it->second.compact_id});
        connection.ids.Release(it->second.compact_id);
        connection.visible.erase(it);
    }
}

void RelevancyEngine::Recalculate(ConnectionId id, Connection& connection) const {
    connection.wanted.clear();
    // Spatial types: one query per observer with the widest reach, then each candidate is judged by
    // its own type. An entity already visible gets the hysteresis margin, so it does not flicker.
    if (query_radius_ > 0.0f) {
        for (const Observer& observer : connection.observers) {
            grid_.ForEachInSphere(
                observer.x, observer.y, observer.z, query_radius_,
                [&](EntityId id, float x, float y, float z) {
                    const TypeRelevancy& rule = config_.types[id.type()];
                    if (rule.mode != RelevancyMode::Spatial || !(rule.range > 0.0f)) {
                        return;
                    }
                    const double dx = static_cast<double>(x) - observer.x;
                    const double dy = static_cast<double>(y) - observer.y;
                    const double dz = static_cast<double>(z) - observer.z;
                    const double distance_squared = dx * dx + dy * dy + dz * dz;
                    const double range = static_cast<double>(rule.range);
                    if (distance_squared <= range * range) {
                        connection.wanted.push_back(id.raw());
                        return;
                    }
                    // Outside the range: only an entity that is already visible may stay, up to
                    // the margin. Most candidates are far beyond it, so the lookup is rare.
                    if (rule.hysteresis_margin > 0.0f) {
                        const double reach = static_cast<double>(rule.range + rule.hysteresis_margin);
                        if (distance_squared <= reach * reach && connection.visible.contains(id.raw())) {
                            connection.wanted.push_back(id.raw());
                        }
                    }
                });
        }
    }
    connection.wanted.insert(connection.wanted.end(), always_.begin(), always_.end());
    connection.wanted.insert(connection.wanted.end(), connection.owned.begin(), connection.owned.end());
    // Groups: inclusion adds entities the type rules would hide; exclusion has the last word.
    connection.included.clear();
    groups_.AppendIncluded(id, connection.included);
    for (const EntityId member : connection.included) {
        connection.wanted.push_back(member.raw());
    }
    SortUnique(connection.wanted);
    connection.wanted.erase(
        std::remove_if(connection.wanted.begin(), connection.wanted.end(),
                       [&](uint64_t raw) { return groups_.IsExcluded(EntityId::FromRaw(raw), id); }),
        connection.wanted.end());
    Reconcile(connection);
}

// Compares `wanted` (sorted) with what the connection sees and records the difference as events:
// entities that left first, then entities that entered, each in ascending id order.
void RelevancyEngine::Reconcile(Connection& connection) {
    ++connection.epoch;
    connection.entering.clear();
    for (const uint64_t raw : connection.wanted) {
        const auto it = connection.visible.find(raw);
        if (it != connection.visible.end()) {
            it->second.epoch = connection.epoch;
        } else {
            connection.entering.push_back(raw);
        }
    }
    connection.leaving.clear();
    for (const auto& [raw, state] : connection.visible) {
        if (state.epoch != connection.epoch) {
            connection.leaving.push_back(raw);
        }
    }
    std::sort(connection.leaving.begin(), connection.leaving.end());
    for (const uint64_t raw : connection.leaving) {
        const auto it = connection.visible.find(raw);
        connection.events.events.push_back(
            {VisibilityEvent::Kind::Leave, EntityId::FromRaw(raw), it->second.compact_id});
        connection.ids.Release(it->second.compact_id);
        connection.visible.erase(it);
    }
    for (const uint64_t raw : connection.entering) {
        const uint32_t compact_id = connection.ids.Acquire();
        if (compact_id == kInvalidCompactId) {
            ++connection.overflow;
            continue;
        }
        connection.visible.emplace(raw, Connection::Visible{compact_id, connection.epoch});
        connection.events.events.push_back(
            {VisibilityEvent::Kind::Enter, EntityId::FromRaw(raw), compact_id});
    }
}

const ConnectionEvents& RelevancyEngine::Events(ConnectionId connection) const {
    if (connection >= config_.max_connections || !connections_[connection]) {
        return kNoEvents;
    }
    return connections_[connection]->events;
}

void RelevancyEngine::ClearEvents(ConnectionId connection) {
    if (connection < config_.max_connections && connections_[connection]) {
        connections_[connection]->events.events.clear();
    }
}

uint32_t RelevancyEngine::CompactIdOf(ConnectionId connection, EntityId entity) const {
    if (connection >= config_.max_connections || !connections_[connection]) {
        return kInvalidCompactId;
    }
    const auto& visible = connections_[connection]->visible;
    const auto it = visible.find(entity.raw());
    return it == visible.end() ? kInvalidCompactId : it->second.compact_id;
}

uint32_t RelevancyEngine::VisibleCount(ConnectionId connection) const {
    if (connection >= config_.max_connections || !connections_[connection]) {
        return 0;
    }
    return static_cast<uint32_t>(connections_[connection]->visible.size());
}

uint64_t RelevancyEngine::VisibleOverflow(ConnectionId connection) const {
    if (connection >= config_.max_connections || !connections_[connection]) {
        return 0;
    }
    return connections_[connection]->overflow;
}

} // namespace tessera::net
