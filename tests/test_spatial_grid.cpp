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

TESSERA_TEST(sphere_queries_match_a_brute_force_search) {
    // The expected answer comes from checking every point, not from another grid query, so a mistake
    // in the cell walk or in the distance test shows up here.
    struct Point {
        EntityId id;
        float x, y, z;
    };
    SpatialGrid grid(GridConfig{64.0f, 64.0f, 64.0f});
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> coord(-500.0f, 500.0f);
    std::vector<Point> points;
    for (uint32_t i = 0; i < 1000; ++i) {
        const Point p{Id(i), coord(rng), coord(rng), coord(rng)};
        grid.Insert(p.id, p.x, p.y, p.z);
        points.push_back(p);
    }
    std::uniform_real_distribution<float> radius(0.0f, 300.0f);
    for (int sphere = 0; sphere < 50; ++sphere) {
        const float cx = coord(rng), cy = coord(rng), cz = coord(rng), r = radius(rng);
        const double limit = static_cast<double>(r) * static_cast<double>(r);
        std::vector<EntityId> expected;
        for (const Point& p : points) {
            const double dx = static_cast<double>(p.x) - cx;
            const double dy = static_cast<double>(p.y) - cy;
            const double dz = static_cast<double>(p.z) - cz;
            if (dx * dx + dy * dy + dz * dz <= limit) {
                expected.push_back(p.id);
            }
        }
        std::vector<EntityId> visited;
        grid.ForEachInSphere(cx, cy, cz, r, [&](EntityId id, float, float, float) { visited.push_back(id); });
        std::vector<EntityId> queried;
        grid.QuerySphere(cx, cy, cz, r, queried);
        std::sort(expected.begin(), expected.end());
        std::sort(visited.begin(), visited.end());
        std::sort(queried.begin(), queried.end());
        TESSERA_CHECK(expected == visited);
        TESSERA_CHECK(expected == queried);
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
    grid.ForEachInSphere(0.0f, 0.0f, 0.0f, 10.0f,
                         [&](EntityId id, float, float, float) { visited.push_back(id); });
    TESSERA_CHECK(Contains(visited, Id(1)));
    TESSERA_CHECK(!Contains(visited, Id(2)));
}

TESSERA_TEST(foreach_in_sphere_handles_negative_coordinates_and_cell_boundaries) {
    SpatialGrid grid(GridConfig{64.0f, 64.0f, 64.0f});
    grid.Insert(Id(1), -64.0f, 0.0f, 0.0f);
    std::vector<EntityId> visited;
    grid.ForEachInSphere(-64.0f, 0.0f, 0.0f, 0.0f,
                         [&](EntityId id, float, float, float) { visited.push_back(id); });
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

// ---- Queries by entity type ---------------------------------------------------------------------

namespace {

EntityId TypedId(uint32_t type, uint32_t index) {
    return EntityId::Make(0, type, index, 0);
}

}  // namespace

TESSERA_TEST(sphere_of_type_matches_a_brute_force_search) {
    struct Point {
        EntityId id;
        float x, y, z;
    };
    SpatialGrid grid(GridConfig{64.0f, 64.0f, 64.0f});
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> coord(-500.0f, 500.0f);
    std::vector<Point> points;
    for (uint32_t i = 0; i < 1000; ++i) {
        const Point p{TypedId(i % 4, i), coord(rng), coord(rng), coord(rng)};
        grid.Insert(p.id, p.x, p.y, p.z);
        points.push_back(p);
    }
    std::uniform_real_distribution<float> radius(0.0f, 300.0f);
    for (int sphere = 0; sphere < 50; ++sphere) {
        const float cx = coord(rng), cy = coord(rng), cz = coord(rng), r = radius(rng);
        const double limit = static_cast<double>(r) * static_cast<double>(r);
        for (uint32_t type = 0; type < 4; ++type) {
            std::vector<EntityId> expected;
            for (const Point& p : points) {
                const double dx = static_cast<double>(p.x) - cx;
                const double dy = static_cast<double>(p.y) - cy;
                const double dz = static_cast<double>(p.z) - cz;
                if (p.id.type() == type && dx * dx + dy * dy + dz * dz <= limit) {
                    expected.push_back(p.id);
                }
            }
            std::vector<EntityId> visited;
            grid.ForEachInSphereOfType(type, cx, cy, cz, r,
                                       [&](EntityId id, float, float, float) { visited.push_back(id); });
            std::sort(expected.begin(), expected.end());
            std::sort(visited.begin(), visited.end());
            TESSERA_CHECK(expected == visited);
        }
    }
}

TESSERA_TEST(sphere_of_type_visits_only_that_type) {
    SpatialGrid grid(GridConfig{10.0f, 10.0f, 10.0f});
    grid.Insert(TypedId(1, 1), 1.0f, 1.0f, 1.0f);
    grid.Insert(TypedId(2, 2), 1.0f, 1.0f, 1.0f);
    std::vector<EntityId> visited;
    grid.ForEachInSphereOfType(1, 0.0f, 0.0f, 0.0f, 10.0f,
                               [&](EntityId id, float, float, float) { visited.push_back(id); });
    TESSERA_CHECK(visited.size() == 1 && visited[0] == TypedId(1, 1));
}

TESSERA_TEST(sphere_of_type_reports_exact_positions_and_includes_the_border) {
    SpatialGrid grid(GridConfig{10.0f, 10.0f, 10.0f});
    grid.Insert(TypedId(1, 1), 1.5f, -2.5f, 3.5f);
    grid.Insert(TypedId(1, 2), 10.0f, 0.0f, 0.0f);
    grid.Insert(TypedId(1, 3), 10.01f, 0.0f, 0.0f);
    int exact = 0;
    int on_border = 0;
    int outside = 0;
    grid.ForEachInSphereOfType(1, 0.0f, 0.0f, 0.0f, 10.0f, [&](EntityId id, float x, float y, float z) {
        if (id == TypedId(1, 1) && x == 1.5f && y == -2.5f && z == 3.5f) {
            ++exact;
        }
        if (id == TypedId(1, 2)) {
            ++on_border;
        }
        if (id == TypedId(1, 3)) {
            ++outside;
        }
    });
    TESSERA_CHECK(exact == 1 && on_border == 1 && outside == 0);
}

TESSERA_TEST(sphere_of_type_ignores_invalid_arguments) {
    SpatialGrid grid(GridConfig{10.0f, 10.0f, 10.0f});
    grid.Insert(TypedId(1, 1), 0.0f, 0.0f, 0.0f);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    int calls = 0;
    const auto count = [&](EntityId, float, float, float) { ++calls; };
    grid.ForEachInSphereOfType(256, 0.0f, 0.0f, 0.0f, 10.0f, count);
    grid.ForEachInSphereOfType(1, nan, 0.0f, 0.0f, 10.0f, count);
    grid.ForEachInSphereOfType(1, 0.0f, 0.0f, 0.0f, inf, count);
    grid.ForEachInSphereOfType(1, 0.0f, 0.0f, 0.0f, -1.0f, count);
    TESSERA_CHECK(calls == 0);
}

TESSERA_TEST(union_of_typed_queries_equals_the_untyped_query) {
    SpatialGrid grid(GridConfig{32.0f, 32.0f, 32.0f});
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> coord(-200.0f, 200.0f);
    for (uint32_t i = 0; i < 500; ++i) {
        grid.Insert(TypedId(i % 5, i), coord(rng), coord(rng), coord(rng));
    }
    std::vector<EntityId> untyped;
    grid.ForEachInSphere(10.0f, -20.0f, 30.0f, 120.0f,
                         [&](EntityId id, float, float, float) { untyped.push_back(id); });
    std::vector<EntityId> united;
    for (uint32_t type = 0; type < 5; ++type) {
        grid.ForEachInSphereOfType(type, 10.0f, -20.0f, 30.0f, 120.0f,
                                   [&](EntityId id, float, float, float) { united.push_back(id); });
    }
    std::sort(untyped.begin(), untyped.end());
    std::sort(united.begin(), united.end());
    TESSERA_CHECK(untyped == united);
}

TESSERA_TEST(removing_the_only_member_of_a_type_keeps_the_other_types_in_the_cell) {
    SpatialGrid grid(GridConfig{100.0f, 100.0f, 100.0f});
    grid.Insert(TypedId(1, 1), 1.0f, 2.0f, 3.0f);
    grid.Insert(TypedId(2, 2), 4.0f, 5.0f, 6.0f);
    TESSERA_CHECK(grid.Remove(TypedId(1, 1)));
    int of_type_1 = 0;
    grid.ForEachInSphereOfType(1, 0.0f, 0.0f, 0.0f, 50.0f,
                               [&](EntityId, float, float, float) { ++of_type_1; });
    TESSERA_CHECK(of_type_1 == 0);
    float x = 0.0f;
    grid.ForEachInSphereOfType(2, 0.0f, 0.0f, 0.0f, 50.0f, [&](EntityId, float px, float, float) { x = px; });
    TESSERA_CHECK(x == 4.0f);
    TESSERA_CHECK(grid.ApplyMoves({PendingMove{TypedId(2, 2), 9.0f, 5.0f, 6.0f}}) == 0);
    grid.ForEachInSphereOfType(2, 0.0f, 0.0f, 0.0f, 50.0f, [&](EntityId, float px, float, float) { x = px; });
    TESSERA_CHECK(x == 9.0f);
    TESSERA_CHECK(grid.CellCount() == 1);
    TESSERA_CHECK(grid.Remove(TypedId(2, 2)));
    TESSERA_CHECK(grid.CellCount() == 0);
}

TESSERA_TEST(swap_removal_inside_a_type_group_keeps_positions_and_indexes_right) {
    SpatialGrid grid(GridConfig{100.0f, 100.0f, 100.0f});
    grid.Insert(TypedId(3, 1), 1.0f, 0.0f, 0.0f);
    grid.Insert(TypedId(3, 2), 2.0f, 0.0f, 0.0f);
    grid.Insert(TypedId(3, 3), 3.0f, 0.0f, 0.0f);
    grid.Insert(TypedId(1, 9), 9.0f, 0.0f, 0.0f);
    TESSERA_CHECK(grid.Remove(TypedId(3, 1)));  // the last member of the group takes its place
    TESSERA_CHECK(grid.ApplyMoves({PendingMove{TypedId(3, 3), 30.0f, 0.0f, 0.0f}}) == 0);
    TESSERA_CHECK(grid.Remove(TypedId(3, 2)));
    float x = 0.0f;
    int count = 0;
    grid.ForEachInSphereOfType(3, 0.0f, 0.0f, 0.0f, 90.0f, [&](EntityId id, float px, float, float) {
        ++count;
        TESSERA_CHECK(id == TypedId(3, 3));
        x = px;
    });
    TESSERA_CHECK(count == 1 && x == 30.0f);
}

// ---- Change feed --------------------------------------------------------------------------------

using tessera::net::GridChange;
using ChangeKind = tessera::net::GridChange::Kind;

TESSERA_TEST(changes_are_not_recorded_by_default) {
    SpatialGrid grid(GridConfig{10.0f, 10.0f, 10.0f});
    grid.Insert(Id(1), 1.0f, 2.0f, 3.0f);
    grid.ApplyMoves({PendingMove{Id(1), 50.0f, 2.0f, 3.0f}});
    grid.Remove(Id(1));
    TESSERA_CHECK(grid.Changes().empty());
}

TESSERA_TEST(insert_and_remove_are_recorded_with_positions) {
    SpatialGrid grid(GridConfig{10.0f, 10.0f, 10.0f});
    grid.SetRecordChanges(true);
    grid.Insert(Id(1), 1.0f, 2.0f, 3.0f);
    grid.Remove(Id(1));
    const auto changes = grid.Changes();
    TESSERA_CHECK(changes.size() == 2);
    TESSERA_CHECK(changes[0].kind == ChangeKind::Inserted && changes[0].id == Id(1));
    TESSERA_CHECK(changes[0].old_x == 1.0f && changes[0].old_y == 2.0f && changes[0].old_z == 3.0f);
    TESSERA_CHECK(changes[0].new_x == 1.0f && changes[0].new_y == 2.0f && changes[0].new_z == 3.0f);
    TESSERA_CHECK(changes[1].kind == ChangeKind::Removed && changes[1].id == Id(1));
    TESSERA_CHECK(changes[1].old_x == 1.0f && changes[1].new_x == 1.0f);
    TESSERA_CHECK(changes[1].old_y == 2.0f && changes[1].new_y == 2.0f);
    TESSERA_CHECK(changes[1].old_z == 3.0f && changes[1].new_z == 3.0f);
}

TESSERA_TEST(move_to_another_cell_is_recorded_with_old_and_new_position) {
    SpatialGrid grid(GridConfig{10.0f, 10.0f, 10.0f});
    grid.Insert(Id(1), 1.0f, 2.0f, 3.0f);
    grid.SetRecordChanges(true);
    TESSERA_CHECK(grid.ApplyMoves({PendingMove{Id(1), 55.0f, 2.0f, -4.0f}}) == 1);
    const auto changes = grid.Changes();
    TESSERA_CHECK(changes.size() == 1);
    TESSERA_CHECK(changes[0].kind == ChangeKind::Moved && changes[0].id == Id(1));
    TESSERA_CHECK(changes[0].old_x == 1.0f && changes[0].old_y == 2.0f && changes[0].old_z == 3.0f);
    TESSERA_CHECK(changes[0].new_x == 55.0f && changes[0].new_y == 2.0f && changes[0].new_z == -4.0f);
}

TESSERA_TEST(move_inside_the_same_cell_is_recorded) {
    SpatialGrid grid(GridConfig{100.0f, 100.0f, 100.0f});
    grid.Insert(Id(1), 1.0f, 2.0f, 3.0f);
    grid.SetRecordChanges(true);
    TESSERA_CHECK(grid.ApplyMoves({PendingMove{Id(1), 2.0f, 2.0f, 3.0f}}) == 0);
    const auto changes = grid.Changes();
    TESSERA_CHECK(changes.size() == 1 && changes[0].kind == ChangeKind::Moved);
    TESSERA_CHECK(changes[0].old_x == 1.0f && changes[0].new_x == 2.0f);
}

TESSERA_TEST(move_to_the_same_position_is_not_recorded) {
    SpatialGrid grid(GridConfig{10.0f, 10.0f, 10.0f});
    grid.Insert(Id(1), 1.0f, 2.0f, 3.0f);
    grid.SetRecordChanges(true);
    grid.ApplyMoves({PendingMove{Id(1), 1.0f, 2.0f, 3.0f}});
    TESSERA_CHECK(grid.Changes().empty());
}

TESSERA_TEST(failed_calls_are_not_recorded) {
    SpatialGrid grid(GridConfig{10.0f, 10.0f, 10.0f});
    grid.Insert(Id(1), 1.0f, 2.0f, 3.0f);
    grid.SetRecordChanges(true);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    TESSERA_CHECK(!grid.Insert(Id(1), 5.0f, 5.0f, 5.0f));  // already there
    TESSERA_CHECK(!grid.Insert(Id(2), nan, 0.0f, 0.0f));   // not finite
    TESSERA_CHECK(!grid.Remove(Id(9)));                    // unknown
    grid.ApplyMoves({PendingMove{Id(9), 1.0f, 1.0f, 1.0f}, PendingMove{Id(1), nan, 0.0f, 0.0f}});
    TESSERA_CHECK(grid.Changes().empty());
}

TESSERA_TEST(changes_keep_their_order_and_clear_changes_empties_them) {
    SpatialGrid grid(GridConfig{10.0f, 10.0f, 10.0f});
    grid.SetRecordChanges(true);
    grid.Insert(Id(1), 1.0f, 0.0f, 0.0f);
    grid.Insert(Id(2), 2.0f, 0.0f, 0.0f);
    grid.ApplyMoves({PendingMove{Id(2), 3.0f, 0.0f, 0.0f}, PendingMove{Id(1), 4.0f, 0.0f, 0.0f}});
    grid.Remove(Id(2));
    const auto changes = grid.Changes();
    TESSERA_CHECK(changes.size() == 5);
    TESSERA_CHECK(changes[0].id == Id(1) && changes[0].kind == ChangeKind::Inserted);
    TESSERA_CHECK(changes[1].id == Id(2) && changes[1].kind == ChangeKind::Inserted);
    TESSERA_CHECK(changes[2].id == Id(2) && changes[2].kind == ChangeKind::Moved);
    TESSERA_CHECK(changes[3].id == Id(1) && changes[3].kind == ChangeKind::Moved);
    TESSERA_CHECK(changes[4].id == Id(2) && changes[4].kind == ChangeKind::Removed);
    grid.ClearChanges();
    TESSERA_CHECK(grid.Changes().empty());
    grid.Insert(Id(3), 5.0f, 0.0f, 0.0f);
    TESSERA_CHECK(grid.Changes().size() == 1);
}

TESSERA_TEST(turning_recording_off_stops_new_records_but_keeps_old_ones) {
    SpatialGrid grid(GridConfig{10.0f, 10.0f, 10.0f});
    grid.SetRecordChanges(true);
    grid.Insert(Id(1), 1.0f, 0.0f, 0.0f);
    grid.SetRecordChanges(false);
    grid.Insert(Id(2), 2.0f, 0.0f, 0.0f);
    TESSERA_CHECK(grid.Changes().size() == 1 && grid.Changes()[0].id == Id(1));
    grid.SetRecordChanges(true);  // turning it on again is harmless
    grid.SetRecordChanges(true);
    grid.Insert(Id(3), 3.0f, 0.0f, 0.0f);
    TESSERA_CHECK(grid.Changes().size() == 2);
}
