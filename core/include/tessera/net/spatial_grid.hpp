// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

#include "tessera/net/entity_id.hpp"

namespace tessera::net {

/// Cell size on each axis, in world units. Sizes are independent so a game can use, for example,
/// 128 m horizontally and 32 m vertically. Y is the vertical axis (as in Godot). A very large
/// `cell_y` gives a flat (2D) grid.
struct GridConfig {
    float cell_x;
    float cell_y;
    float cell_z;
};

/// Integer coordinates of a cell.
struct CellCoord {
    int32_t x;
    int32_t y;
    int32_t z;

    friend bool operator==(const CellCoord& a, const CellCoord& b) noexcept {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
};

/// A position change collected during the simulation, applied later in a single step.
struct PendingMove {
    EntityId id;
    float x;
    float y;
    float z;
};

/// One change made to the grid, as reported by `SpatialGrid::Changes()`. For `Inserted` the old position
/// equals the new one; for `Removed` the new position equals the old one.
struct GridChange {
    enum class Kind : uint8_t { Inserted, Moved, Removed };

    Kind kind;
    EntityId id;
    float old_x, old_y, old_z;
    float new_x, new_y, new_z;
};

}  // namespace tessera::net

template <>
struct std::hash<tessera::net::CellCoord> {
    size_t operator()(const tessera::net::CellCoord& c) const noexcept {
        // Large odd multipliers spread neighbouring cells across buckets.
        const uint64_t h = static_cast<uint64_t>(static_cast<uint32_t>(c.x)) * 73856093ull ^
                           static_cast<uint64_t>(static_cast<uint32_t>(c.y)) * 19349663ull ^
                           static_cast<uint64_t>(static_cast<uint32_t>(c.z)) * 83492791ull;
        return static_cast<size_t>(h);
    }
};

namespace tessera::net {

/// A sparse 3D grid: only cells that contain entities exist. It answers "who is near this point"
/// and is the base for deciding what each player can see.
///
/// Thread safety: the read-only calls (`CellOf`, `WouldChangeCell`, the queries) may run in parallel
/// with each other. Anything that changes the grid must run alone.
class SpatialGrid {
public:
    /// Throws std::invalid_argument if a cell size is not a positive, finite number.
    explicit SpatialGrid(GridConfig config);

    /// The cell containing a point: the floor of position divided by cell size, so negative
    /// coordinates and exact boundaries are handled consistently. Non-finite input maps to cell 0.
    CellCoord CellOf(float x, float y, float z) const noexcept;

    /// Adds an entity. Returns false if it is already in the grid or the position is not finite.
    bool Insert(EntityId id, float x, float y, float z);

    /// Removes an entity. Returns false if it is not in the grid.
    bool Remove(EntityId id);

    /// True if moving the entity to this position would put it in a different cell. False for
    /// unknown entities and for non-finite positions. Safe to call in parallel.
    bool WouldChangeCell(EntityId id, float x, float y, float z) const noexcept;

    /// Applies moves in the given order and updates the stored positions. Returns how many entities
    /// changed cell. Unknown ids and non-finite positions are ignored.
    size_t ApplyMoves(const std::vector<PendingMove>& moves);

    size_t EntityCount() const noexcept { return entries_.size(); }
    size_t CellCount() const noexcept { return cells_.size(); }

    /// Starts or stops keeping a list of the changes made to the grid (off by default, and then nothing
    /// is kept). While on, every `Insert`, `Remove` and every move in `ApplyMoves` that changes a
    /// position is added to the list, in the order it happened, including moves inside one cell. Calls
    /// that fail, and moves to the same position, are not recorded. Whoever turns it on must call
    /// `ClearChanges` regularly, or the list grows without limit. Only one consumer should read it.
    void SetRecordChanges(bool enabled) noexcept { record_changes_ = enabled; }

    /// Like `SetRecordChanges(true)`, but several users can share the recording: it stays on until every
    /// user that called `RetainChangeRecording` has called `ReleaseChangeRecording`, and the last release
    /// turns it off and empties the list. This lets one reader replace another without losing changes.
    void RetainChangeRecording() noexcept {
        ++recording_users_;
        record_changes_ = true;
    }
    void ReleaseChangeRecording() noexcept {
        if (recording_users_ > 0 && --recording_users_ == 0) {
            record_changes_ = false;
            changes_.clear();
        }
    }

    /// The changes recorded since the last `ClearChanges`.
    std::span<const GridChange> Changes() const noexcept { return changes_; }

    /// Empties the list of recorded changes.
    void ClearChanges() noexcept { changes_.clear(); }

    /// Appends the entities of one cell to `out`.
    void QueryCell(CellCoord cell, std::vector<EntityId>& out) const;

    /// Appends the entities inside the box (borders included), using their exact positions.
    void QueryBox(float min_x, float min_y, float min_z, float max_x, float max_y, float max_z,
                  std::vector<EntityId>& out) const;

    /// Appends the entities within `radius` of a point (borders included), using exact distances.
    void QuerySphere(float cx, float cy, float cz, float radius, std::vector<EntityId>& out) const;

    /// Calls `fn(EntityId id, float x, float y, float z)` for every entity within `radius` of a point
    /// (borders included), using exact distances. Unlike `QuerySphere` it allocates nothing and hands
    /// over the stored position, so hot loops can filter without a second lookup. A non-finite
    /// centre or radius, or a negative radius, visits nothing. Safe to call in parallel.
    template <class F>
    void ForEachInSphere(float cx, float cy, float cz, float radius, F&& fn) const {
        if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(cz) || !std::isfinite(radius) ||
            radius < 0.0f) {
            return;
        }
        const double limit = static_cast<double>(radius) * static_cast<double>(radius);
        ForEachEntryInRange(
            CellOf(cx - radius, cy - radius, cz - radius), CellOf(cx + radius, cy + radius, cz + radius),
            [&](const CellMember& m) {
                const double dx = static_cast<double>(m.x) - cx;
                const double dy = static_cast<double>(m.y) - cy;
                const double dz = static_cast<double>(m.z) - cz;
                return dx * dx + dy * dy + dz * dz <= limit;
            },
            [&](const CellMember& m) { fn(m.id, m.x, m.y, m.z); });
    }

    /// Like `ForEachInSphere`, but visits only entities whose `EntityId::type()` is `type`, and reads only
    /// that type's members of each cell. Asking once per type, each with its own radius, is much cheaper
    /// than one wide query when types have very different reaches. A `type` above `EntityId::kMaxType`
    /// visits nothing.
    template <class F>
    void ForEachInSphereOfType(uint32_t type, float cx, float cy, float cz, float radius, F&& fn) const {
        if (type > EntityId::kMaxType || !std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(cz) ||
            !std::isfinite(radius) || radius < 0.0f) {
            return;
        }
        const double limit = static_cast<double>(radius) * static_cast<double>(radius);
        ForEachEntryInRange(
            CellOf(cx - radius, cy - radius, cz - radius), CellOf(cx + radius, cy + radius, cz + radius),
            [&](const CellMember& m) {
                const double dx = static_cast<double>(m.x) - cx;
                const double dy = static_cast<double>(m.y) - cy;
                const double dz = static_cast<double>(m.z) - cz;
                return dx * dx + dy * dy + dz * dz <= limit;
            },
            [&](const CellMember& m) { fn(m.id, m.x, m.y, m.z); }, static_cast<int64_t>(type));
    }

private:
    /// What a cell stores for each of its entities. The position is kept here, next to the id, so a
    /// query reads cells front to back without looking each entity up in a hash table.
    struct CellMember {
        EntityId id;
        float x, y, z;
    };

    /// The members of one cell that share an entity type. Keeping types apart lets a query skip the types
    /// it does not need.
    struct TypeGroup {
        uint32_t type;
        std::vector<CellMember> members;
    };

    /// A cell: its type groups, ascending by type (a cell has few types).
    struct Cell {
        std::vector<TypeGroup> groups;
    };

    /// What the grid remembers per entity, to find it again in its cell.
    struct Entry {
        float x, y, z;
        CellCoord cell;
        uint32_t index_in_cell; // index inside the group of the entity's type
    };

    static TypeGroup* FindGroup(Cell& cell, uint32_t type);
    void AddToCell(CellCoord cell, EntityId id, Entry& entry);
    void RemoveFromCell(EntityId id, const Entry& entry);

    /// Calls `visit(member)` for each entity in the cells of [low, high] whose cell member passes
    /// `accept`; `only_type` is the entity type to read, or -1 for every type. Walks the occupied cells
    /// when there are fewer of them than cells in the range.
    template <class Accept, class Visit>
    void ForEachEntryInRange(CellCoord low, CellCoord high, Accept&& accept, Visit&& visit,
                             int64_t only_type = -1) const {
        const auto scan = [&](const std::vector<CellMember>& members) {
            for (const CellMember& member : members) {
                if (accept(member)) {
                    visit(member);
                }
            }
        };
        const auto handle = [&](const Cell& cell) {
            for (const TypeGroup& group : cell.groups) {
                if (only_type < 0 || group.type == static_cast<uint32_t>(only_type)) {
                    scan(group.members);
                }
                if (only_type >= 0 && group.type >= static_cast<uint32_t>(only_type)) {
                    break; // groups are ascending by type, so the wanted one is behind us
                }
            }
        };
        const double volume = (static_cast<double>(high.x) - low.x + 1) *
                              (static_cast<double>(high.y) - low.y + 1) *
                              (static_cast<double>(high.z) - low.z + 1);
        if (volume > static_cast<double>(cells_.size())) {
            for (const auto& [coord, members] : cells_) {
                if (coord.x >= low.x && coord.x <= high.x && coord.y >= low.y && coord.y <= high.y &&
                    coord.z >= low.z && coord.z <= high.z) {
                    handle(members);
                }
            }
            return;
        }
        for (int64_t cx = low.x; cx <= high.x; ++cx) {
            for (int64_t cy = low.y; cy <= high.y; ++cy) {
                for (int64_t cz = low.z; cz <= high.z; ++cz) {
                    const auto it = cells_.find(CellCoord{static_cast<int32_t>(cx), static_cast<int32_t>(cy),
                                                          static_cast<int32_t>(cz)});
                    if (it != cells_.end()) {
                        handle(it->second);
                    }
                }
            }
        }
    }

    GridConfig config_;
    std::unordered_map<CellCoord, Cell> cells_;
    std::unordered_map<uint64_t, Entry> entries_;
    bool record_changes_ = false;
    uint32_t recording_users_ = 0;
    std::vector<GridChange> changes_;
};

}  // namespace tessera::net
