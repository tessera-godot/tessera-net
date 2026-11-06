// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/spatial_grid.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace tessera::net {

namespace {

bool IsPositiveFinite(float value) {
    return std::isfinite(value) && value > 0.0f;
}

bool IsFinite(float x, float y, float z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

int32_t CellIndex(float position, float cell_size) {
    const double cell = std::floor(static_cast<double>(position) / static_cast<double>(cell_size));
    constexpr double kLow = static_cast<double>(std::numeric_limits<int32_t>::min());
    constexpr double kHigh = static_cast<double>(std::numeric_limits<int32_t>::max());
    return static_cast<int32_t>(std::clamp(cell, kLow, kHigh));
}

}  // namespace

SpatialGrid::SpatialGrid(GridConfig config) : config_(config) {
    if (!IsPositiveFinite(config.cell_x) || !IsPositiveFinite(config.cell_y) || !IsPositiveFinite(config.cell_z)) {
        throw std::invalid_argument("SpatialGrid: every cell size must be a positive, finite number");
    }
}

CellCoord SpatialGrid::CellOf(float x, float y, float z) const noexcept {
    if (!IsFinite(x, y, z)) {
        return CellCoord{0, 0, 0};
    }
    return CellCoord{CellIndex(x, config_.cell_x), CellIndex(y, config_.cell_y), CellIndex(z, config_.cell_z)};
}

bool SpatialGrid::Insert(EntityId id, float x, float y, float z) {
    if (!IsFinite(x, y, z) || entries_.contains(id.raw())) {
        return false;
    }
    Entry entry{x, y, z, CellOf(x, y, z), 0};
    AddToCell(entry.cell, id, entry);
    entries_.emplace(id.raw(), entry);
    return true;
}

bool SpatialGrid::Remove(EntityId id) {
    const auto it = entries_.find(id.raw());
    if (it == entries_.end()) {
        return false;
    }
    RemoveFromCell(id, it->second);
    entries_.erase(it);
    return true;
}

bool SpatialGrid::WouldChangeCell(EntityId id, float x, float y, float z) const noexcept {
    if (!IsFinite(x, y, z)) {
        return false;
    }
    const auto it = entries_.find(id.raw());
    if (it == entries_.end()) {
        return false;
    }
    return !(CellOf(x, y, z) == it->second.cell);
}

size_t SpatialGrid::ApplyMoves(const std::vector<PendingMove>& moves) {
    size_t changed = 0;
    for (const PendingMove& move : moves) {
        if (!IsFinite(move.x, move.y, move.z)) {
            continue;
        }
        const auto it = entries_.find(move.id.raw());
        if (it == entries_.end()) {
            continue;
        }
        Entry& entry = it->second;
        entry.x = move.x;
        entry.y = move.y;
        entry.z = move.z;
        const CellCoord target = CellOf(move.x, move.y, move.z);
        if (!(target == entry.cell)) {
            RemoveFromCell(move.id, entry);
            AddToCell(target, move.id, entry);
            ++changed;
        } else {
            // Same cell: the copy of the position kept in the cell must follow.
            Cell& cell = cells_.find(entry.cell)->second;
            CellMember& member = FindGroup(cell, move.id.type())->members[entry.index_in_cell];
            member.x = move.x;
            member.y = move.y;
            member.z = move.z;
        }
    }
    return changed;
}

SpatialGrid::TypeGroup* SpatialGrid::FindGroup(Cell& cell, uint32_t type) {
    for (TypeGroup& group : cell.groups) {
        if (group.type == type) {
            return &group;
        }
    }
    return nullptr;
}

void SpatialGrid::AddToCell(CellCoord coord, EntityId id, Entry& entry) {
    Cell& cell = cells_[coord];
    const uint32_t type = id.type();
    // Keep the groups ascending by type; a cell holds only a handful of them.
    auto at = std::lower_bound(cell.groups.begin(), cell.groups.end(), type,
                               [](const TypeGroup& group, uint32_t t) { return group.type < t; });
    if (at == cell.groups.end() || at->type != type) {
        at = cell.groups.insert(at, TypeGroup{type, {}});
    }
    entry.cell = coord;
    entry.index_in_cell = static_cast<uint32_t>(at->members.size());
    at->members.push_back(CellMember{id, entry.x, entry.y, entry.z});
}

void SpatialGrid::RemoveFromCell(EntityId id, const Entry& entry) {
    const auto cell_it = cells_.find(entry.cell);
    Cell& cell = cell_it->second;
    TypeGroup* group = FindGroup(cell, id.type());
    std::vector<CellMember>& members = group->members;
    const CellMember moved = members.back();
    members[entry.index_in_cell] = moved;
    members.pop_back();
    if (entry.index_in_cell < members.size()) {
        entries_.find(moved.id.raw())->second.index_in_cell = entry.index_in_cell;
    }
    if (members.empty()) {
        cell.groups.erase(cell.groups.begin() + (group - cell.groups.data()));
    }
    if (cell.groups.empty()) {
        cells_.erase(cell_it);
    }
}

void SpatialGrid::QueryCell(CellCoord cell, std::vector<EntityId>& out) const {
    const auto it = cells_.find(cell);
    if (it != cells_.end()) {
        for (const TypeGroup& group : it->second.groups) {
            for (const CellMember& member : group.members) {
                out.push_back(member.id);
            }
        }
    }
}

void SpatialGrid::QueryBox(float min_x, float min_y, float min_z, float max_x, float max_y, float max_z,
                           std::vector<EntityId>& out) const {
    if (!IsFinite(min_x, min_y, min_z) || !IsFinite(max_x, max_y, max_z)) {
        return;
    }
    ForEachEntryInRange(
        CellOf(min_x, min_y, min_z), CellOf(max_x, max_y, max_z),
        [&](const CellMember& m) {
            return m.x >= min_x && m.x <= max_x && m.y >= min_y && m.y <= max_y && m.z >= min_z &&
                   m.z <= max_z;
        },
        [&](const CellMember& m) { out.push_back(m.id); });
}

void SpatialGrid::QuerySphere(float cx, float cy, float cz, float radius, std::vector<EntityId>& out) const {
    ForEachInSphere(cx, cy, cz, radius, [&](EntityId id, float, float, float) { out.push_back(id); });
}

}  // namespace tessera::net
