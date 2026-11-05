// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

#include "tessera/net/spatial_grid.hpp"
#include "test_support.hpp"

using tessera::net::CellCoord;
using tessera::net::EntityId;
using tessera::net::GridConfig;
using tessera::net::PendingMove;
using tessera::net::SpatialGrid;

namespace {

EntityId Id(uint32_t index) {
    return EntityId::Make(0, 1, index, 0);
}

bool Contains(const std::vector<EntityId>& ids, EntityId id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

}  // namespace

TESSERA_TEST(cell_of_uses_floor_for_negative_coordinates) {
    const SpatialGrid grid(GridConfig{10.0f, 10.0f, 10.0f});
    TESSERA_CHECK(grid.CellOf(-0.1f, 0, 0).x == -1);
    TESSERA_CHECK(grid.CellOf(-10.0f, 0, 0).x == -1);
    TESSERA_CHECK(grid.CellOf(-10.1f, 0, 0).x == -2);
}

TESSERA_TEST(cell_boundary_belongs_to_the_higher_cell) {
    const SpatialGrid grid(GridConfig{10.0f, 10.0f, 10.0f});
    TESSERA_CHECK(grid.CellOf(10.0f, 0, 0).x == 1);
    TESSERA_CHECK(grid.CellOf(9.999f, 0, 0).x == 0);
}

TESSERA_TEST(axis_sizes_are_independent) {
    const SpatialGrid grid(GridConfig{100.0f, 100.0f, 10.0f});
    TESSERA_CHECK(grid.CellOf(0, 0, 25.0f).z == 2);
    TESSERA_CHECK(grid.CellOf(150.0f, 0, 0).x == 1);
    TESSERA_CHECK(grid.CellOf(0, 150.0f, 0).y == 1);
}

TESSERA_TEST(invalid_config_throws) {
    int thrown = 0;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const GridConfig& bad : {GridConfig{0.0f, 1.0f, 1.0f}, GridConfig{1.0f, -1.0f, 1.0f},
                                  GridConfig{1.0f, 1.0f, nan}}) {
        try {
            const SpatialGrid grid(bad);
            (void)grid;
        } catch (const std::invalid_argument&) {
            ++thrown;
        }
    }
    TESSERA_CHECK(thrown == 3);
}

TESSERA_TEST(insert_move_remove_track_counts_and_cells) {
    SpatialGrid grid(GridConfig{10.0f, 10.0f, 10.0f});
    TESSERA_CHECK(grid.Insert(Id(1), 5, 5, 5));
    TESSERA_CHECK(!grid.Insert(Id(1), 5, 5, 5));
    TESSERA_CHECK(grid.EntityCount() == 1);
    TESSERA_CHECK(grid.CellCount() == 1);
    TESSERA_CHECK(grid.ApplyMoves({PendingMove{Id(1), 25, 5, 5}}) == 1);
    TESSERA_CHECK(grid.CellCount() == 1);  // the old cell was dropped when it became empty
    TESSERA_CHECK(grid.Remove(Id(1)));
    TESSERA_CHECK(grid.EntityCount() == 0);
    TESSERA_CHECK(grid.CellCount() == 0);
}

TESSERA_TEST(apply_moves_reports_only_real_cell_changes) {
    SpatialGrid grid(GridConfig{10.0f, 10.0f, 10.0f});
    grid.Insert(Id(1), 1, 1, 1);
    TESSERA_CHECK(grid.ApplyMoves({PendingMove{Id(1), 2, 2, 2}}) == 0);
    std::vector<EntityId> found;
    grid.QuerySphere(2, 2, 2, 0.5f, found);
    TESSERA_CHECK(Contains(found, Id(1)));  // the stored position was still updated
}

TESSERA_TEST(would_change_cell_matches_apply_moves) {
    SpatialGrid grid(GridConfig{10.0f, 10.0f, 10.0f});
    grid.Insert(Id(1), 1, 1, 1);
    TESSERA_CHECK(!grid.WouldChangeCell(Id(1), 9.0f, 1, 1));
    TESSERA_CHECK(grid.WouldChangeCell(Id(1), 10.0f, 1, 1));
    TESSERA_CHECK(grid.ApplyMoves({PendingMove{Id(1), 10.0f, 1, 1}}) == 1);
}

TESSERA_TEST(query_box_and_sphere_filter_by_exact_position) {
    SpatialGrid grid(GridConfig{10.0f, 10.0f, 10.0f});
    grid.Insert(Id(1), 9.0f, 0, 0);  // same cell as the origin, but 9 away
    std::vector<EntityId> found;
    grid.QuerySphere(0, 0, 0, 5.0f, found);
    TESSERA_CHECK(found.empty());
    grid.QuerySphere(0, 0, 0, 10.0f, found);
    TESSERA_CHECK(Contains(found, Id(1)));
    found.clear();
    grid.QueryBox(-1, -1, -1, 5, 5, 5, found);
    TESSERA_CHECK(found.empty());
    grid.QueryBox(-1, -1, -1, 9.5f, 5, 5, found);
    TESSERA_CHECK(Contains(found, Id(1)));
}

TESSERA_TEST(query_cell_lists_the_cell_members) {
    SpatialGrid grid(GridConfig{10.0f, 10.0f, 10.0f});
    grid.Insert(Id(1), 1, 1, 1);
    grid.Insert(Id(2), 2, 2, 2);
    grid.Insert(Id(3), 50, 1, 1);
    std::vector<EntityId> found;
    grid.QueryCell(CellCoord{0, 0, 0}, found);
    TESSERA_CHECK(found.size() == 2);
    TESSERA_CHECK(Contains(found, Id(1)) && Contains(found, Id(2)));
}

TESSERA_TEST(unknown_ids_are_ignored_by_move_and_remove) {
    SpatialGrid grid(GridConfig{10.0f, 10.0f, 10.0f});
    TESSERA_CHECK(grid.ApplyMoves({PendingMove{Id(9), 1, 1, 1}}) == 0);
    TESSERA_CHECK(!grid.Remove(Id(9)));
    TESSERA_CHECK(!grid.WouldChangeCell(Id(9), 1, 1, 1));
    TESSERA_CHECK(grid.EntityCount() == 0);
}

TESSERA_TEST(non_finite_positions_are_rejected) {
    SpatialGrid grid(GridConfig{10.0f, 10.0f, 10.0f});
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    TESSERA_CHECK(!grid.Insert(Id(1), nan, 0, 0));
    TESSERA_CHECK(!grid.Insert(Id(1), 0, inf, 0));
    TESSERA_CHECK(grid.EntityCount() == 0);
    grid.Insert(Id(2), 1, 1, 1);
    TESSERA_CHECK(grid.ApplyMoves({PendingMove{Id(2), nan, 0, 0}}) == 0);
    TESSERA_CHECK(!grid.WouldChangeCell(Id(2), inf, 0, 0));
    std::vector<EntityId> found;
    grid.QuerySphere(1, 1, 1, 0.5f, found);
    TESSERA_CHECK(Contains(found, Id(2)));  // the entity kept its last valid position
}

TESSERA_TEST(single_vertical_layer_behaves_like_2d) {
    SpatialGrid grid(GridConfig{10.0f, 1.0e9f, 10.0f});  // Y is the vertical axis
    grid.Insert(Id(1), 1, 0.0f, 1);
    grid.Insert(Id(2), 2, 500.0f, 2);
    TESSERA_CHECK(grid.CellCount() == 1);
}
