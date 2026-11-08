// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tessera/net/entity_id.hpp"
#include "tessera/net/job_system.hpp"
#include "tessera/net/relevancy/relevancy_config.hpp"
#include "tessera/net/relevancy/relevancy_groups.hpp"
#include "tessera/net/relevancy/relevancy_types.hpp"
#include "tessera/net/spatial_grid.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace tessera::net {

/// Decides, for each connection, which entities it should receive, and reports changes as events.
///
/// What a connection sees comes from three sources, combined in this order: the entity's type (spatial
/// range around the connection's observers, visible to everyone, or visible to its owner only), the
/// groups that include entities, and finally the groups that exclude entities (exclusion always wins).
/// Every connection also gets a compact id for each visible entity, to use on the wire.
///
/// To stay cheap the engine does not look at everything around a player every tick. Each observer has an
/// *anchor*: the position it was last evaluated from. A full evaluation of a connection happens when it
/// is new, when an observer has moved `observer_tolerance` or more from its anchor, or when something
/// that is not about distance changes (owners, groups, always-relevant entities). In between, only the
/// entities that changed in the grid are looked at again.
///
/// Phases (see `TickLoop`): everything that changes structure - creating and destroying entities,
/// owners, groups, connections, observers - runs in the Structure phase. `Update` runs in the Network
/// phase.
class RelevancyEngine {
public:
    /// `grid`, `groups` and `jobs` must outlive the engine. `groups` is not constant because the
    /// engine removes destroyed entities and removed connections from it; `grid` is not constant because
    /// the engine asks it to record the changes made to it (`SpatialGrid::RetainChangeRecording`) and empties
    /// that list at the end of every `Update`. Only one engine may use a grid. Throws
    /// std::invalid_argument if the configuration is invalid.
    RelevancyEngine(const RelevancyConfig& config, SpatialGrid& grid, RelevancyGroups& groups,
                    IJobSystem& jobs);

    /// Stops the grid from recording changes.
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
    /// not finite. An empty list is valid: the connection then sees no spatial entities. The connection
    /// is evaluated again from the new points if the number of observers changed or any observer is
    /// `observer_tolerance` or more away from its anchor.
    bool SetObservers(ConnectionId connection, std::span<const Observer> observers);

    /// Tells the engine an entity exists. Needed for always-relevant and owner-only types; spatial
    /// entities are found through the grid.
    void OnEntityCreated(EntityId entity);

    /// Tells the engine an entity no longer exists. Every connection that saw it gets a `Destroy`
    /// event in the next `Update`. The entity is also removed from every group. The entity must no
    /// longer be in the grid when `Update` runs: a spatial entity that is still there would be reported
    /// as destroyed and then seen again.
    void OnEntityDestroyed(EntityId entity);

    /// Sets the owner of an owner-only entity. False if the entity is not known to the engine, its
    /// type is not owner-only, or the connection does not exist.
    bool SetOwner(EntityId entity, ConnectionId owner);

    /// Brings every connection up to date and appends what changed to its events. Call once per tick.
    /// At most `max_reanchors_per_tick` connections are evaluated in full per call (all of them if that
    /// is 0); the ones that have waited longest go first. If `connections_processed` is not null it
    /// receives the number of connections evaluated in full.
    void Update(uint64_t tick, uint32_t* connections_processed = nullptr);

    /// How many connections were due for a full evaluation in the last `Update` but had to wait because
    /// of `max_reanchors_per_tick`.
    uint32_t DeferredReanchors() const noexcept {
        return deferred_reanchors_;
    }

    /// Changes since the last `ClearEvents`, in order. An empty list for an unknown connection.
    const ConnectionEvents& Events(ConnectionId connection) const;

    /// Call after the events of a connection were used.
    void ClearEvents(ConnectionId connection);

    /// The compact id of an entity on a connection, or `kInvalidCompactId` if it is not visible there.
    uint32_t CompactIdOf(ConnectionId connection, EntityId entity) const;

    /// How many entities the connection sees now.
    uint32_t VisibleCount(ConnectionId connection) const;

    /// How many times an entity stayed hidden because the connection's compact ids were all in use
    /// (added up over every evaluation).
    uint64_t VisibleOverflow(ConnectionId connection) const;

    /// The positions the connection was last evaluated from, one per observer. For diagnostics and
    /// tests; an empty list for an unknown connection.
    std::span<const Observer> Anchors(ConnectionId connection) const;

private:
    struct Connection;

    /// A spatial entity type that can be seen at all, with the distances that matter for it.
    struct SpatialType {
        uint32_t type;
        double range; // an entity this close is visible
        double reach; // an entity already visible stays visible up to this far (range + margin)
        float radius; // what to ask the grid for (reach, as a float)
        bool has_margin;
    };

    /// What one tick's worth of grid changes says about an entity: where it was before the tick and where it
    /// is after, with several recorded changes folded into one.
    struct ChangedEntity {
        uint64_t raw;
        bool existed_before;
        bool exists_after;
        float old_x, old_y, old_z; // meaningful when `existed_before`
        float new_x, new_y, new_z; // meaningful when `exists_after`
    };

    void MarkAllForFullEvaluation();
    void FoldGridChanges();
    void BuildChangeIndex();
    /// Whether a change, before or after it, is within the reach of its type from any of the anchors.
    bool IsNear(const ChangedEntity& change, std::span<const Observer> anchors) const;
    /// Appends the changes filed around `anchor` that are within reach of any of `anchors`. Returns how many it
    /// looked at (counted only in measurement builds; 0 otherwise).
    uint64_t GatherNearbyChanges(const Observer& anchor, std::span<const Observer> anchors,
                                 std::vector<uint32_t>& out) const;
    void EvaluateChanges(ConnectionId id, Connection& connection) const;
    void ApplyDestructions(Connection& connection) const;
    void EvaluateInFull(ConnectionId id, Connection& connection) const;
    void FinishTick(Connection& connection) const;

    RelevancyConfig config_;
    SpatialGrid& grid_;
    RelevancyGroups& groups_;
    IJobSystem& jobs_;
    std::vector<SpatialType> spatial_types_;
    std::array<int16_t, 256> spatial_index_{}; // entity type -> index in spatial_types_, or -1
    std::vector<ChangedEntity> changed_;       // this tick's folded grid changes, ascending by raw id
    std::vector<std::pair<uint64_t, uint32_t>> change_order_; // scratch for folding

    // A coarse grid of this engine's own over the folded changes (each change is filed under the cell it came
    // from and the cell it went to), so a connection finds the few changes near its anchors without reading
    // them all. Cells are as large as the widest reach. It is a chain per hash bucket; two cells that share a
    // bucket only add candidates, which are checked by distance anyway.
    float index_cell_ = 0.0f;
    std::vector<uint32_t> index_head_;  // bucket -> first entry, or none
    std::vector<uint32_t> index_next_;  // entry -> next entry in the same bucket, or none
    std::vector<uint32_t> index_entry_; // entry -> index into changed_
    uint64_t seen_group_version_ = 0;
    uint32_t deferred_reanchors_ = 0;

    std::vector<std::unique_ptr<Connection>> connections_; // indexed by ConnectionId
    std::vector<ConnectionId> active_;                     // existing connections, ascending
    std::vector<ConnectionId> pending_;                 // scratch: connections waiting for a full evaluation
    std::vector<uint64_t> always_;                      // raw ids of always-relevant entities, ascending
    std::unordered_map<uint64_t, ConnectionId> owners_; // owner-only entities and their owner
    std::vector<uint64_t> destroyed_;                   // destroyed since the last Update
};

} // namespace tessera::net
