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
    RemoveFromCell(it->second);
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
            RemoveFromCell(entry);
            AddToCell(target, move.id, entry);
            ++changed;
        }
    }
    return changed;
}

void SpatialGrid::AddToCell(CellCoord cell, EntityId id, Entry& entry) {
    std::vector<EntityId>& members = cells_[cell];
    entry.cell = cell;
    entry.index_in_cell = static_cast<uint32_t>(members.size());
    members.push_back(id);
}

void SpatialGrid::RemoveFromCell(const Entry& entry) {
    const auto cell_it = cells_.find(entry.cell);
    std::vector<EntityId>& members = cell_it->second;
    const EntityId moved = members.back();
    members[entry.index_in_cell] = moved;
    members.pop_back();
    if (entry.index_in_cell < members.size()) {
        entries_.find(moved.raw())->second.index_in_cell = entry.index_in_cell;
    }
    if (members.empty()) {
        cells_.erase(cell_it);
    }
}

void SpatialGrid::QueryCell(CellCoord cell, std::vector<EntityId>& out) const {
    const auto it = cells_.find(cell);
    if (it != cells_.end()) {
        out.insert(out.end(), it->second.begin(), it->second.end());
    }
}

void SpatialGrid::QueryBox(float min_x, float min_y, float min_z, float max_x, float max_y, float max_z,
                           std::vector<EntityId>& out) const {
    if (!IsFinite(min_x, min_y, min_z) || !IsFinite(max_x, max_y, max_z)) {
        return;
    }
    ForEachEntryInRange(
        CellOf(min_x, min_y, min_z), CellOf(max_x, max_y, max_z),
        [&](const Entry& e) {
            return e.x >= min_x && e.x <= max_x && e.y >= min_y && e.y <= max_y && e.z >= min_z && e.z <= max_z;
        },
        [&](EntityId id, const Entry&) { out.push_back(id); });
}

void SpatialGrid::QuerySphere(float cx, float cy, float cz, float radius, std::vector<EntityId>& out) const {
    ForEachInSphere(cx, cy, cz, radius, [&](EntityId id, float, float, float) { out.push_back(id); });
}

}  // namespace tessera::net
