// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
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

private:
    /// What a cell stores for each of its entities. The position is kept here, next to the id, so a
    /// query reads cells front to back without looking each entity up in a hash table.
    struct CellMember {
        EntityId id;
        float x, y, z;
    };

    /// What the grid remembers per entity, to find it again in its cell.
    struct Entry {
        float x, y, z;
        CellCoord cell;
        uint32_t index_in_cell;
    };

    void AddToCell(CellCoord cell, EntityId id, Entry& entry);
    void RemoveFromCell(const Entry& entry);

    /// Calls `visit(member)` for each entity in the cells of [low, high] whose cell member passes
    /// `accept`. Walks the occupied cells when there are fewer of them than cells in the range.
    template <class Accept, class Visit>
    void ForEachEntryInRange(CellCoord low, CellCoord high, Accept&& accept, Visit&& visit) const {
        const auto handle = [&](const std::vector<CellMember>& members) {
            for (const CellMember& member : members) {
                if (accept(member)) {
                    visit(member);
                }
            }
        };
        const double volume = (static_cast<double>(high.x) - low.x + 1) * (static_cast<double>(high.y) - low.y + 1) *
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
    std::unordered_map<CellCoord, std::vector<CellMember>> cells_;
    std::unordered_map<uint64_t, Entry> entries_;
};

}  // namespace tessera::net
