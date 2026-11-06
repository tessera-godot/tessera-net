// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

#include "tessera/net/entity_id.hpp"
#include "tessera/net/job_system.hpp"
#include "tessera/net/relevancy/relevancy_config.hpp"
#include "tessera/net/relevancy/relevancy_groups.hpp"
#include "tessera/net/relevancy/relevancy_types.hpp"
#include "tessera/net/spatial_grid.hpp"

namespace tessera::net {

/// Decides, for each connection, which entities it should receive, and reports changes as events.
///
/// What a connection sees comes from three sources, combined in this order: the entity's type (spatial
/// range around the connection's observers, visible to everyone, or visible to its owner only), the
/// groups that include entities, and finally the groups that exclude entities (exclusion always wins).
/// Every connection also gets a compact id for each visible entity, to use on the wire.
///
/// Phases (see `TickLoop`): everything that changes structure - creating and destroying entities,
/// owners, groups, connections, observers - runs in the Structure phase. `Update` runs in the Network
/// phase and reads the grid, the groups and the configuration without changing them.
class RelevancyEngine {
public:
    /// `grid`, `groups` and `jobs` must outlive the engine. `groups` is not constant because the
    /// engine removes destroyed entities and removed connections from it. Throws
    /// std::invalid_argument if the configuration is invalid.
    RelevancyEngine(const RelevancyConfig& config, const SpatialGrid& grid, RelevancyGroups& groups,
                    IJobSystem& jobs);
    ~RelevancyEngine();

    RelevancyEngine(const RelevancyEngine&) = delete;
    RelevancyEngine& operator=(const RelevancyEngine&) = delete;

    /// False if the id is out of range or already in use.
    bool AddConnection(ConnectionId connection);

    /// Frees the connection's state. The entities it owned are left without an owner. False if the
    /// connection does not exist.
    bool RemoveConnection(ConnectionId connection);

    /// Replaces the points the connection sees from. False, with no change, if the connection does
    /// not exist, there are more observers than `max_observers_per_connection`, or a coordinate is
    /// not finite. An empty list is valid: the connection then sees no spatial entities.
    bool SetObservers(ConnectionId connection, std::span<const Observer> observers);

    /// Tells the engine an entity exists. Needed for always-relevant and owner-only types; spatial
    /// entities are found through the grid.
    void OnEntityCreated(EntityId entity);

    /// Tells the engine an entity no longer exists. Every connection that saw it gets a `Destroy`
    /// event in the next `Update`, whatever its recalculation schedule.
    void OnEntityDestroyed(EntityId entity);

    /// Sets the owner of an owner-only entity. False if the entity is not known to the engine, its
    /// type is not owner-only, or the connection does not exist.
    bool SetOwner(EntityId entity, ConnectionId owner);

    /// Recomputes what each due connection sees and appends the changes to its events. A connection
    /// is due when `connection % recalc_interval_ticks == tick % recalc_interval_ticks`, so the work is
    /// spread across ticks. Destructions are handled for every connection on every call. Call once
    /// per tick. If `connections_processed` is not null it receives the number of connections that
    /// were recalculated.
    void Update(uint64_t tick, uint32_t* connections_processed = nullptr);

    /// Changes since the last `ClearEvents`, in order. An empty list for an unknown connection.
    const ConnectionEvents& Events(ConnectionId connection) const;

    /// Call after the events of a connection were used.
    void ClearEvents(ConnectionId connection);

    /// The compact id of an entity on a connection, or `kInvalidCompactId` if it is not visible there.
    uint32_t CompactIdOf(ConnectionId connection, EntityId entity) const;

    /// How many entities the connection sees now.
    uint32_t VisibleCount(ConnectionId connection) const;

    /// How many times an entity stayed hidden because the connection's compact ids were all in use
    /// (added up over every recalculation).
    uint64_t VisibleOverflow(ConnectionId connection) const;

private:
    struct Connection;

    void Recalculate(ConnectionId id, Connection& connection) const;
    static void Reconcile(Connection& connection);

    RelevancyConfig config_;
    const SpatialGrid& grid_;
    RelevancyGroups& groups_;
    IJobSystem& jobs_;

    std::vector<std::unique_ptr<Connection>> connections_;  // indexed by ConnectionId
    std::vector<ConnectionId> active_;                      // existing connections, ascending
    std::vector<uint64_t> always_;                          // raw ids of always-relevant entities, ascending
    std::unordered_map<uint64_t, ConnectionId> owners_;     // owner-only entities and their owner
    std::vector<uint64_t> destroyed_;                       // destroyed since the last Update
};

}  // namespace tessera::net
