// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/relevancy/relevancy_engine.hpp"

#include "tessera/net/profile.hpp"
#include "tessera/net/relevancy/compact_ids.hpp"
#include "tessera/net/relevancy/visible_set.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace tessera::net {

namespace {

constexpr ConnectionId kNoOwner = 0xFFFFFFFFu;
constexpr uint64_t kNoTick = std::numeric_limits<uint64_t>::max();
constexpr uint32_t kNoEntry = 0xFFFFFFFFu;

const ConnectionEvents kNoEvents{};

void SortUnique(std::vector<uint64_t>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

/// Merges the ascending list `extra` into the ascending list `base`, keeping it ascending.
void MergeSorted(std::vector<uint64_t>& base, const std::vector<uint64_t>& extra,
                 std::vector<uint64_t>& scratch) {
    if (extra.empty()) {
        return;
    }
    scratch.resize(base.size() + extra.size());
    std::merge(base.begin(), base.end(), extra.begin(), extra.end(), scratch.begin());
    base.swap(scratch);
}

int32_t IndexCellOf(double position, double cell) {
    const double index = std::floor(position / cell);
    constexpr double kLow = static_cast<double>(std::numeric_limits<int32_t>::min());
    constexpr double kHigh = static_cast<double>(std::numeric_limits<int32_t>::max());
    return static_cast<int32_t>(std::clamp(index, kLow, kHigh));
}

double DistanceSquared(const Observer& anchor, float x, float y, float z) {
    const double dx = static_cast<double>(x) - anchor.x;
    const double dy = static_cast<double>(y) - anchor.y;
    const double dz = static_cast<double>(z) - anchor.z;
    return dx * dx + dy * dy + dz * dz;
}

} // namespace

/// Everything the engine remembers about one connection. A connection's work touches only its own
/// state, which is why connections can be processed in parallel without locks.
struct RelevancyEngine::Connection {
    explicit Connection(uint32_t id_capacity) : ids(id_capacity) {}

    CompactIdAllocator ids;
    VisibleSet visible;
    ConnectionEvents events;
    std::vector<Observer> observers; // where the observers are now
    std::vector<Observer> anchors;   // where they were the last time the connection was evaluated in full
    std::vector<uint64_t> owned;     // owner-only entities this connection owns

    bool needs_full = false;          // a full evaluation is due
    bool selected = false;            // chosen for a full evaluation in the current Update
    bool overflowed = false;          // an entity was left out because the ids ran out
    uint64_t pending_since = kNoTick; // the tick at which the full evaluation became due
    uint64_t overflow = 0;

    // Scratch vectors reused every tick: once they have grown they stop allocating.
    std::vector<uint64_t> wanted;
    std::vector<EntityId> included;
    std::vector<uint64_t> destroy_raws;
    std::vector<uint64_t> enter_raws; // ascending
    std::vector<uint64_t> leave_raws; // ascending
    std::vector<uint64_t> feed_enter; // ascending, from the grid changes of this tick
    std::vector<uint64_t> feed_leave; // ascending
    std::vector<uint64_t> merge_scratch;
    std::vector<uint32_t> candidates; // indexes into changed_ of the changes near the anchors
    std::vector<VisibleEntry> enter_entries;
};

RelevancyEngine::RelevancyEngine(const RelevancyConfig& config, SpatialGrid& grid, RelevancyGroups& groups,
                                 IJobSystem& jobs)
    : config_(config), grid_(grid), groups_(groups), jobs_(jobs) {
    ValidateRelevancyConfig(config_);
    connections_.resize(config_.max_connections);
    spatial_index_.fill(-1);
    for (uint32_t type = 0; type < config_.types.size(); ++type) {
        const TypeRelevancy& rule = config_.types[type];
        if (rule.mode == RelevancyMode::Spatial && rule.range > 0.0f) {
            spatial_index_[type] = static_cast<int16_t>(spatial_types_.size());
            const float radius = rule.range + rule.hysteresis_margin;
            spatial_types_.push_back(SpatialType{type, static_cast<double>(rule.range),
                                                 static_cast<double>(radius), radius,
                                                 rule.hysteresis_margin > 0.0f});
        }
    }
    for (const SpatialType& type : spatial_types_) {
        index_cell_ = std::max(index_cell_, type.radius);
    }
    seen_group_version_ = groups_.version();
    grid_.ClearChanges(); // start from now: what was in the grid before is found by the first evaluations
    grid_.RetainChangeRecording();
}

RelevancyEngine::~RelevancyEngine() {
    grid_.ReleaseChangeRecording();
}

bool RelevancyEngine::AddConnection(ConnectionId connection) {
    if (connection >= config_.max_connections || connections_[connection]) {
        return false;
    }
    connections_[connection] = std::make_unique<Connection>(config_.max_visible_per_connection);
    connections_[connection]->needs_full = true;
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
    // Taking the connection out of the groups changes their version. If the game had not changed the groups
    // since the last Update, that change is ours and needs no re-evaluation: nobody can see the connection.
    const uint64_t before = groups_.version();
    groups_.RemoveConnection(connection);
    if (before == seen_group_version_) {
        seen_group_version_ = groups_.version();
    }
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
    Connection& state = *connections_[connection];
    // A different number of observers, or one that has gone `observer_tolerance` or more from its anchor,
    // means the connection has to be looked at again from the new points.
    bool moved = observers.size() != state.anchors.size();
    const double tolerance = static_cast<double>(config_.observer_tolerance);
    for (size_t i = 0; !moved && i < observers.size(); ++i) {
        const double dx = static_cast<double>(observers[i].x) - state.anchors[i].x;
        const double dy = static_cast<double>(observers[i].y) - state.anchors[i].y;
        const double dz = static_cast<double>(observers[i].z) - state.anchors[i].z;
        moved = dx * dx + dy * dy + dz * dz >= tolerance * tolerance;
    }
    state.observers.assign(observers.begin(), observers.end());
    if (moved) {
        state.needs_full = true;
    }
    return true;
}

void RelevancyEngine::OnEntityCreated(EntityId entity) {
    switch (config_.types[entity.type()].mode) {
    case RelevancyMode::AlwaysRelevant: {
        const auto it = std::lower_bound(always_.begin(), always_.end(), entity.raw());
        if (it == always_.end() || *it != entity.raw()) {
            always_.insert(it, entity.raw());
            MarkAllForFullEvaluation(); // everybody has to see it
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
    // Leaving the groups changes their version. If the game had not changed the groups since the last
    // Update, that change is ours: the entity is going away anyway and the Destroy event covers it.
    const uint64_t before = groups_.version();
    groups_.RemoveEntity(entity);
    if (before == seen_group_version_) {
        seen_group_version_ = groups_.version();
    }
    destroyed_.push_back(entity.raw()); // the Destroy events are written by the next Update
}

bool RelevancyEngine::SetOwner(EntityId entity, ConnectionId owner) {
    const auto it = owners_.find(entity.raw());
    if (it == owners_.end() || owner >= config_.max_connections || !connections_[owner]) {
        return false;
    }
    if (it->second != kNoOwner) {
        Connection& previous = *connections_[it->second];
        previous.owned.erase(std::remove(previous.owned.begin(), previous.owned.end(), entity.raw()),
                             previous.owned.end());
        previous.needs_full = true;
    }
    it->second = owner;
    connections_[owner]->owned.push_back(entity.raw());
    connections_[owner]->needs_full = true;
    return true;
}

void RelevancyEngine::MarkAllForFullEvaluation() {
    for (const ConnectionId id : active_) {
        connections_[id]->needs_full = true;
    }
}

void RelevancyEngine::Update(uint64_t tick, uint32_t* connections_processed) {
    SortUnique(destroyed_);
    if (groups_.version() != seen_group_version_) {
        MarkAllForFullEvaluation();
        seen_group_version_ = groups_.version();
    }
    // Decide, serially and in a way that does not depend on the number of workers, which connections get a
    // full evaluation in this tick: the ones that have waited longest, up to the cap.
    pending_.clear();
    for (const ConnectionId id : active_) {
        Connection& connection = *connections_[id];
        // A connection that left entities out because its ids ran out tries again once an id is free
        // (counting the ids that are given back at the start of this tick).
        if (connection.overflowed &&
            connection.ids.InUse() - connection.ids.PendingRelease() < connection.ids.Capacity()) {
            connection.needs_full = true;
        }
        connection.selected = false;
        if (connection.needs_full) {
            if (connection.pending_since == kNoTick) {
                connection.pending_since = tick;
            }
            pending_.push_back(id);
        }
    }
    std::sort(pending_.begin(), pending_.end(), [this](ConnectionId a, ConnectionId b) {
        const uint64_t since_a = connections_[a]->pending_since;
        const uint64_t since_b = connections_[b]->pending_since;
        return since_a != since_b ? since_a < since_b : a < b;
    });
    const size_t cap = config_.max_reanchors_per_tick;
    const size_t chosen = cap == 0 ? pending_.size() : std::min(cap, pending_.size());
    for (size_t i = 0; i < chosen; ++i) {
        connections_[pending_[i]]->selected = true;
    }
    deferred_reanchors_ = static_cast<uint32_t>(pending_.size() - chosen);
    if (connections_processed != nullptr) {
        *connections_processed = static_cast<uint32_t>(chosen);
    }
    {
        TESSERA_PROFILE_SCOPE(RelFold);
        FoldGridChanges();
    }
    TESSERA_PROFILE_COUNT(Changes, changed_.size());
    {
        TESSERA_PROFILE_SCOPE(RelIndex);
        BuildChangeIndex();
    }

    if (!active_.empty()) {
        TESSERA_PROFILE_SCOPE(RelRegion);
        jobs_.ParallelFor(static_cast<uint32_t>(active_.size()), 1,
                          [&](uint32_t begin, uint32_t end, uint32_t) {
                              for (uint32_t i = begin; i < end; ++i) {
                                  const ConnectionId id = active_[i];
                                  Connection& connection = *connections_[id];
                                  connection.ids.BeginTick();
                                  {
                                      TESSERA_PROFILE_SCOPE(RelDestroy);
                                      ApplyDestructions(connection);
                                  }
                                  connection.enter_raws.clear();
                                  connection.leave_raws.clear();
                                  if (connection.selected) {
                                      TESSERA_PROFILE_SCOPE(RelFull);
                                      EvaluateInFull(id, connection);
                                  }
                                  EvaluateChanges(id, connection); // timed inside, in its own stages
                                  {
                                      TESSERA_PROFILE_SCOPE(RelFinish);
                                      FinishTick(connection);
                                  }
                              }
                          });
    }
    destroyed_.clear();
    grid_.ClearChanges();
}

// Destructions are reported at once. The id stays reserved until the next tick, like any other id that
// is given up.
void RelevancyEngine::ApplyDestructions(Connection& connection) const {
    connection.destroy_raws.clear();
    for (const uint64_t raw : destroyed_) {
        const VisibleEntry* entry = connection.visible.Find(raw);
        if (entry == nullptr) {
            continue;
        }
        connection.events.events.push_back(
            {VisibilityEvent::Kind::Destroy, EntityId::FromRaw(raw), entry->compact_id});
        connection.ids.Release(entry->compact_id);
        connection.destroy_raws.push_back(raw);
    }
    connection.visible.Apply(connection.destroy_raws, {});
}

// Works out everything the connection should see from its observers' current positions, which become its
// new anchors, and records what differs from what it sees now as entities to enter and to leave.
void RelevancyEngine::EvaluateInFull(ConnectionId id, Connection& connection) const {
    connection.anchors = connection.observers;
    connection.overflowed = false; // FinishTick sets it again if the ids run out once more
    connection.wanted.clear();
    // Spatial types: one query per type at that type's own reach. An entity already visible gets the
    // hysteresis margin, so it does not flicker at the edge. Most candidates are inside the range, so the
    // lookup of the visible set only happens in the margin.
    for (const Observer& anchor : connection.anchors) {
        for (const SpatialType& type : spatial_types_) {
            grid_.ForEachInSphereOfType(type.type, anchor.x, anchor.y, anchor.z, type.radius,
                                        [&](EntityId entity, float x, float y, float z) {
                                            const double dx = static_cast<double>(x) - anchor.x;
                                            const double dy = static_cast<double>(y) - anchor.y;
                                            const double dz = static_cast<double>(z) - anchor.z;
                                            const double distance_squared = dx * dx + dy * dy + dz * dz;
                                            if (distance_squared <= type.range * type.range ||
                                                (type.has_margin &&
                                                 distance_squared <= type.reach * type.reach &&
                                                 connection.visible.Contains(entity.raw()))) {
                                                connection.wanted.push_back(entity.raw());
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

    // Compare the two sorted lists: what is visible but not wanted leaves, what is wanted but not visible
    // enters.
    const std::span<const VisibleEntry> visible = connection.visible.Entries();
    size_t v = 0;
    size_t w = 0;
    while (v < visible.size() || w < connection.wanted.size()) {
        if (w == connection.wanted.size() || (v < visible.size() && visible[v].raw < connection.wanted[w])) {
            connection.leave_raws.push_back(visible[v++].raw);
        } else if (v == visible.size() || connection.wanted[w] < visible[v].raw) {
            connection.enter_raws.push_back(connection.wanted[w++]);
        } else {
            ++v;
            ++w;
        }
    }
    connection.needs_full = false;
    connection.pending_since = kNoTick;
}

// Folds the changes the grid recorded during this tick into one record per entity of a spatial type:
// where it was before the tick (if it existed) and where it is after (if it still does). An entity that
// appeared and disappeared within the tick leaves no record.
void RelevancyEngine::FoldGridChanges() {
    changed_.clear();
    const std::span<const GridChange> changes = grid_.Changes();
    if (changes.empty()) {
        return;
    }
    change_order_.clear();
    for (size_t i = 0; i < changes.size(); ++i) {
        if (spatial_index_[changes[i].id.type()] >= 0) {
            change_order_.emplace_back(changes[i].id.raw(), static_cast<uint32_t>(i));
        }
    }
    std::sort(change_order_.begin(), change_order_.end()); // by entity, then in the order it happened
    for (size_t first = 0; first < change_order_.size();) {
        size_t last = first;
        while (last + 1 < change_order_.size() &&
               change_order_[last + 1].first == change_order_[first].first) {
            ++last;
        }
        const GridChange& earliest = changes[change_order_[first].second];
        const GridChange& latest = changes[change_order_[last].second];
        const bool existed_before = earliest.kind != GridChange::Kind::Inserted;
        const bool exists_after = latest.kind != GridChange::Kind::Removed;
        if (existed_before || exists_after) {
            changed_.push_back(ChangedEntity{change_order_[first].first, existed_before, exists_after,
                                             earliest.old_x, earliest.old_y, earliest.old_z, latest.new_x,
                                             latest.new_y, latest.new_z});
        }
        first = last + 1;
    }
}

// Files every folded change under the index cell it came from and the one it went to.
void RelevancyEngine::BuildChangeIndex() {
    index_entry_.clear();
    index_next_.clear();
    if (changed_.empty() || index_cell_ <= 0.0f) {
        index_head_.clear();
        return;
    }
    size_t buckets = 16;
    while (buckets < changed_.size() * 4) {
        buckets <<= 1;
    }
    index_head_.assign(buckets, kNoEntry);
    const double cell = static_cast<double>(index_cell_);
    const auto file_under = [&](float x, float y, float z, uint32_t change) {
        const CellCoord coord{IndexCellOf(x, cell), IndexCellOf(y, cell), IndexCellOf(z, cell)};
        const size_t bucket = std::hash<CellCoord>{}(coord) & (buckets - 1);
        index_entry_.push_back(change);
        index_next_.push_back(index_head_[bucket]);
        index_head_[bucket] = static_cast<uint32_t>(index_entry_.size() - 1);
    };
    for (uint32_t i = 0; i < changed_.size(); ++i) {
        const ChangedEntity& change = changed_[i];
        if (change.exists_after) {
            file_under(change.new_x, change.new_y, change.new_z, i);
        }
        if (change.existed_before) {
            const bool same_cell = change.exists_after &&
                                   IndexCellOf(change.old_x, cell) == IndexCellOf(change.new_x, cell) &&
                                   IndexCellOf(change.old_y, cell) == IndexCellOf(change.new_y, cell) &&
                                   IndexCellOf(change.old_z, cell) == IndexCellOf(change.new_z, cell);
            if (!same_cell) {
                file_under(change.old_x, change.old_y, change.old_z, i);
            }
        }
    }
}

// A change matters to a connection if the entity was within the reach of its type from one of the anchors
// before the change, or is after it. Only entities of spatial types are folded into the changes, so the type
// always has a reach.
bool RelevancyEngine::IsNear(const ChangedEntity& change, std::span<const Observer> anchors) const {
    const SpatialType& type =
        spatial_types_[static_cast<size_t>(spatial_index_[EntityId::FromRaw(change.raw).type()])];
    const double reach_squared = type.reach * type.reach;
    for (const Observer& anchor : anchors) {
        if (change.existed_before &&
            DistanceSquared(anchor, change.old_x, change.old_y, change.old_z) <= reach_squared) {
            return true;
        }
        if (change.exists_after &&
            DistanceSquared(anchor, change.new_x, change.new_y, change.new_z) <= reach_squared) {
            return true;
        }
    }
    return false;
}

// Appends the changes filed in the cells that the widest reach around the anchor can touch and that are
// within reach of one of the anchors, so that most of what the cells hold (changes far from every anchor)
// never has to be sorted. A change may be appended more than once (filed under two cells, or found from two
// anchors); the caller sorts and removes duplicates.
uint64_t RelevancyEngine::GatherNearbyChanges(const Observer& anchor, std::span<const Observer> anchors,
                                              std::vector<uint32_t>& out) const {
    uint64_t looked_at = 0;
    if (index_head_.empty()) {
        return looked_at;
    }
    const double cell = static_cast<double>(index_cell_);
    const double reach = cell * (1.0 + 1e-7); // a hair more, so rounding cannot drop a boundary cell
    const int32_t low_x = IndexCellOf(static_cast<double>(anchor.x) - reach, cell);
    const int32_t high_x = IndexCellOf(static_cast<double>(anchor.x) + reach, cell);
    const int32_t low_y = IndexCellOf(static_cast<double>(anchor.y) - reach, cell);
    const int32_t high_y = IndexCellOf(static_cast<double>(anchor.y) + reach, cell);
    const int32_t low_z = IndexCellOf(static_cast<double>(anchor.z) - reach, cell);
    const int32_t high_z = IndexCellOf(static_cast<double>(anchor.z) + reach, cell);
    const size_t mask = index_head_.size() - 1;
    for (int64_t cx = low_x; cx <= high_x; ++cx) {
        for (int64_t cy = low_y; cy <= high_y; ++cy) {
            for (int64_t cz = low_z; cz <= high_z; ++cz) {
                const CellCoord coord{static_cast<int32_t>(cx), static_cast<int32_t>(cy),
                                      static_cast<int32_t>(cz)};
                for (uint32_t entry = index_head_[std::hash<CellCoord>{}(coord)&mask]; entry != kNoEntry;
                     entry = index_next_[entry]) {
                    const uint32_t index = index_entry_[entry];
#if TESSERA_PROFILE
                    ++looked_at;
#endif
                    if (IsNear(changed_[index], anchors)) {
                        out.push_back(index);
                    }
                }
            }
        }
    }
    return looked_at;
}

// Looks again only at the entities that changed in the grid and are within reach of one of the connection
// anchors, before or after the change. Everything else cannot change what the connection sees.
void RelevancyEngine::EvaluateChanges(ConnectionId id, Connection& connection) const {
    connection.feed_enter.clear();
    connection.feed_leave.clear();
    if (connection.anchors.empty() || changed_.empty()) {
        return;
    }
    connection.candidates.clear();
#if TESSERA_PROFILE
    uint64_t looked_at = 0; // added to the profile once, after the gather
#endif
    {
        TESSERA_PROFILE_SCOPE(RelGather);
        for (const Observer& anchor : connection.anchors) {
#if TESSERA_PROFILE
            looked_at += GatherNearbyChanges(anchor, connection.anchors, connection.candidates);
#else
            GatherNearbyChanges(anchor, connection.anchors, connection.candidates);
#endif
        }
    }
#if TESSERA_PROFILE
    TESSERA_PROFILE_COUNT(Candidates, looked_at);
#endif
    TESSERA_PROFILE_COUNT(Sorted, connection.candidates.size());
    {
        TESSERA_PROFILE_SCOPE(RelSortUnique);
        // The changes are ordered by entity id, so ordering their indexes keeps the results in id order.
        std::sort(connection.candidates.begin(), connection.candidates.end());
        connection.candidates.erase(std::unique(connection.candidates.begin(), connection.candidates.end()),
                                    connection.candidates.end());
    }
#if TESSERA_PROFILE
    uint64_t near_count = 0; // added to the profile once, after the loop
#endif
    { // timed as the stage "filter"; the merges after it are not part of it
        TESSERA_PROFILE_SCOPE(RelFilter);
        for (const uint32_t index : connection.candidates) {
            const ChangedEntity& change = changed_[index];
            const EntityId entity = EntityId::FromRaw(change.raw);
            const SpatialType& type = spatial_types_[static_cast<size_t>(spatial_index_[entity.type()])];
            const double reach_squared = type.reach * type.reach;
            bool near = false;
            double nearest_after = std::numeric_limits<double>::infinity();
            for (const Observer& anchor : connection.anchors) {
                if (change.existed_before &&
                    DistanceSquared(anchor, change.old_x, change.old_y, change.old_z) <= reach_squared) {
                    near = true;
                }
                if (change.exists_after) {
                    const double after = DistanceSquared(anchor, change.new_x, change.new_y, change.new_z);
                    nearest_after = std::min(nearest_after, after);
                    near = near || after <= reach_squared;
                }
            }
            if (!near) {
                continue;
            }
#if TESSERA_PROFILE
            ++near_count;
#endif
            // What the connection sees of this entity, counting what the full evaluation of this tick
            // decided.
            const bool listed = connection.visible.Contains(change.raw);
            const bool leaving =
                std::binary_search(connection.leave_raws.begin(), connection.leave_raws.end(), change.raw);
            const bool entering =
                std::binary_search(connection.enter_raws.begin(), connection.enter_raws.end(), change.raw);
            const bool visible_now = (listed && !leaving) || entering;
            const bool by_distance =
                change.exists_after && (nearest_after <= type.range * type.range ||
                                        (type.has_margin && visible_now && nearest_after <= reach_squared));
            const bool wanted =
                (by_distance || groups_.IsIncludedFor(entity, id)) && !groups_.IsExcluded(entity, id);
            if (wanted && !visible_now) {
                connection.feed_enter.push_back(change.raw);
            } else if (!wanted && visible_now) {
                connection.feed_leave.push_back(change.raw);
            }
        }
    }
#if TESSERA_PROFILE
    TESSERA_PROFILE_COUNT(Near, near_count);
#endif
    MergeSorted(connection.enter_raws, connection.feed_enter, connection.merge_scratch);
    MergeSorted(connection.leave_raws, connection.feed_leave, connection.merge_scratch);
}

// Turns the decisions of this tick into events and into the new visible set: entities that left first,
// then entities that entered, each in ascending id order.
void RelevancyEngine::FinishTick(Connection& connection) const {
    for (const uint64_t raw : connection.leave_raws) {
        const VisibleEntry* entry = connection.visible.Find(raw);
        connection.events.events.push_back(
            {VisibilityEvent::Kind::Leave, EntityId::FromRaw(raw), entry->compact_id});
        connection.ids.Release(entry->compact_id);
    }
    connection.enter_entries.clear();
    for (const uint64_t raw : connection.enter_raws) {
        const uint32_t compact_id = connection.ids.Acquire();
        if (compact_id == kInvalidCompactId) {
            ++connection.overflow;
            connection.overflowed = true;
            continue;
        }
        connection.enter_entries.push_back({raw, compact_id});
        connection.events.events.push_back(
            {VisibilityEvent::Kind::Enter, EntityId::FromRaw(raw), compact_id});
    }
    connection.visible.Apply(connection.leave_raws, connection.enter_entries);
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
    const VisibleEntry* entry = connections_[connection]->visible.Find(entity.raw());
    return entry == nullptr ? kInvalidCompactId : entry->compact_id;
}

uint32_t RelevancyEngine::VisibleCount(ConnectionId connection) const {
    if (connection >= config_.max_connections || !connections_[connection]) {
        return 0;
    }
    return static_cast<uint32_t>(connections_[connection]->visible.Size());
}

uint64_t RelevancyEngine::VisibleOverflow(ConnectionId connection) const {
    if (connection >= config_.max_connections || !connections_[connection]) {
        return 0;
    }
    return connections_[connection]->overflow;
}

std::span<const Observer> RelevancyEngine::Anchors(ConnectionId connection) const {
    if (connection >= config_.max_connections || !connections_[connection]) {
        return {};
    }
    return connections_[connection]->anchors;
}

} // namespace tessera::net
