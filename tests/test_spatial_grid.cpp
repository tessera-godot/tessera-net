// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
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

TESSERA_TEST(foreach_in_sphere_matches_query_sphere) {
    SpatialGrid grid(GridConfig{64.0f, 64.0f, 64.0f});
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> coord(-500.0f, 500.0f);
    for (uint32_t i = 0; i < 1000; ++i) {
        grid.Insert(Id(i), coord(rng), coord(rng), coord(rng));
    }
    std::uniform_real_distribution<float> radius(0.0f, 300.0f);
    for (int sphere = 0; sphere < 50; ++sphere) {
        const float cx = coord(rng), cy = coord(rng), cz = coord(rng), r = radius(rng);
        std::vector<EntityId> expected;
        grid.QuerySphere(cx, cy, cz, r, expected);
        std::vector<EntityId> visited;
        grid.ForEachInSphere(cx, cy, cz, r, [&](EntityId id, float, float, float) { visited.push_back(id); });
        std::sort(expected.begin(), expected.end());
        std::sort(visited.begin(), visited.end());
        TESSERA_CHECK(expected == visited);
    }
}

TESSERA_TEST(foreach_in_sphere_reports_exact_positions) {
    SpatialGrid grid(GridConfig{10.0f, 10.0f, 10.0f});
    grid.Insert(Id(1), 1.5f, -2.5f, 3.5f);
    int calls = 0;
    grid.ForEachInSphere(0.0f, 0.0f, 0.0f, 10.0f, [&](EntityId id, float x, float y, float z) {
        ++calls;
        TESSERA_CHECK(id == Id(1));
        TESSERA_CHECK(x == 1.5f && y == -2.5f && z == 3.5f);
    });
    TESSERA_CHECK(calls == 1);
}

TESSERA_TEST(foreach_in_sphere_includes_the_border) {
    SpatialGrid grid(GridConfig{10.0f, 10.0f, 10.0f});
    grid.Insert(Id(1), 10.0f, 0.0f, 0.0f);
    grid.Insert(Id(2), 10.01f, 0.0f, 0.0f);
    std::vector<EntityId> visited;
    grid.ForEachInSphere(0.0f, 0.0f, 0.0f, 10.0f, [&](EntityId id, float, float, float) { visited.push_back(id); });
    TESSERA_CHECK(Contains(visited, Id(1)));
    TESSERA_CHECK(!Contains(visited, Id(2)));
}

TESSERA_TEST(foreach_in_sphere_handles_negative_coordinates_and_cell_boundaries) {
    SpatialGrid grid(GridConfig{64.0f, 64.0f, 64.0f});
    grid.Insert(Id(1), -64.0f, 0.0f, 0.0f);
    std::vector<EntityId> visited;
    grid.ForEachInSphere(-64.0f, 0.0f, 0.0f, 0.0f, [&](EntityId id, float, float, float) { visited.push_back(id); });
    TESSERA_CHECK(visited.size() == 1 && visited[0] == Id(1));
}

TESSERA_TEST(foreach_in_sphere_ignores_invalid_arguments) {
    SpatialGrid grid(GridConfig{10.0f, 10.0f, 10.0f});
    grid.Insert(Id(1), 0.0f, 0.0f, 0.0f);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    int calls = 0;
    const auto count = [&](EntityId, float, float, float) { ++calls; };
    grid.ForEachInSphere(nan, 0.0f, 0.0f, 10.0f, count);
    grid.ForEachInSphere(0.0f, 0.0f, 0.0f, inf, count);
    grid.ForEachInSphere(0.0f, 0.0f, 0.0f, -1.0f, count);
    TESSERA_CHECK(calls == 0);
}

TESSERA_TEST(moving_inside_the_same_cell_updates_the_position_seen_by_queries) {
    SpatialGrid grid(GridConfig{10.0f, 10.0f, 10.0f});
    grid.Insert(Id(1), 1.0f, 0.0f, 0.0f);
    TESSERA_CHECK(grid.ApplyMoves({PendingMove{Id(1), 9.0f, 0.0f, 0.0f}}) == 0);  // same cell
    float seen_x = -1.0f;
    int calls = 0;
    grid.ForEachInSphere(9.0f, 0.0f, 0.0f, 0.0f, [&](EntityId, float x, float, float) {
        seen_x = x;
        ++calls;
    });
    TESSERA_CHECK(calls == 1 && seen_x == 9.0f);
    std::vector<EntityId> old_spot;
    grid.QuerySphere(1.0f, 0.0f, 0.0f, 0.5f, old_spot);
    TESSERA_CHECK(old_spot.empty());
}

TESSERA_TEST(removing_an_entity_keeps_the_positions_of_the_others_correct) {
    SpatialGrid grid(GridConfig{100.0f, 100.0f, 100.0f});
    grid.Insert(Id(1), 1.0f, 2.0f, 3.0f);
    grid.Insert(Id(2), 4.0f, 5.0f, 6.0f);
    grid.Insert(Id(3), 7.0f, 8.0f, 9.0f);
    TESSERA_CHECK(grid.Remove(Id(1)));  // the last member is swapped into the freed slot
    int seen = 0;
    grid.ForEachInSphere(0.0f, 0.0f, 0.0f, 50.0f, [&](EntityId id, float x, float y, float z) {
        ++seen;
        if (id == Id(2)) {
            TESSERA_CHECK(x == 4.0f && y == 5.0f && z == 6.0f);
        } else {
            TESSERA_CHECK(id == Id(3));
            TESSERA_CHECK(x == 7.0f && y == 8.0f && z == 9.0f);
        }
    });
    TESSERA_CHECK(seen == 2);
    // The swapped entity can still be moved and removed correctly.
    TESSERA_CHECK(grid.ApplyMoves({PendingMove{Id(3), 20.0f, 0.0f, 0.0f}}) == 0);
    TESSERA_CHECK(grid.Remove(Id(2)));
    float x_of_last = 0.0f;
    grid.ForEachInSphere(0.0f, 0.0f, 0.0f, 50.0f, [&](EntityId id, float x, float, float) {
        TESSERA_CHECK(id == Id(3));
        x_of_last = x;
    });
    TESSERA_CHECK(x_of_last == 20.0f);
}

TESSERA_TEST(moving_to_another_cell_keeps_the_position_in_the_new_cell) {
    SpatialGrid grid(GridConfig{10.0f, 10.0f, 10.0f});
    grid.Insert(Id(1), 1.0f, 0.0f, 0.0f);
    grid.Insert(Id(2), 2.0f, 0.0f, 0.0f);
    TESSERA_CHECK(grid.ApplyMoves({PendingMove{Id(1), 55.5f, 1.0f, -2.0f}}) == 1);
    float x = 0.0f, y = 0.0f, z = 0.0f;
    grid.ForEachInSphere(55.5f, 1.0f, -2.0f, 0.0f, [&](EntityId id, float px, float py, float pz) {
        TESSERA_CHECK(id == Id(1));
        x = px;
        y = py;
        z = pz;
    });
    TESSERA_CHECK(x == 55.5f && y == 1.0f && z == -2.0f);
    std::vector<EntityId> left_behind;
    grid.QuerySphere(2.0f, 0.0f, 0.0f, 1.0f, left_behind);
    TESSERA_CHECK(left_behind.size() == 1 && left_behind[0] == Id(2));
}
