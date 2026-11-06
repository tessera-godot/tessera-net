// SPDX-License-Identifier: Apache-2.0
#include "relevancy_test_support.hpp"
#include "test_support.hpp"

#include <memory>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <set>
#include <span>
#include <stdexcept>

using namespace relevancy_test;
using Kind = tessera::net::VisibilityEvent::Kind;

TESSERA_TEST(engine_rejects_invalid_config) {
    RelevancyConfig config = MakeConfig();
    config.max_connections = 0;
    bool threw = false;
    try {
        Fixture fixture(config);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    TESSERA_CHECK(threw);
}

TESSERA_TEST(add_and_remove_connection_validate_ids) {
    RelevancyConfig config = MakeConfig();
    config.max_connections = 4;
    Fixture f(config);
    TESSERA_CHECK(f.engine.AddConnection(0));
    TESSERA_CHECK(!f.engine.AddConnection(0));
    TESSERA_CHECK(!f.engine.AddConnection(4));
    TESSERA_CHECK(!f.engine.RemoveConnection(3));
    TESSERA_CHECK(f.engine.RemoveConnection(0));
    TESSERA_CHECK(!f.engine.RemoveConnection(0));
}

TESSERA_TEST(always_relevant_entity_enters_in_the_first_update_after_the_connection_is_added) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(1);
    const EntityId e = MakeId(kGlobalType, 1);
    f.engine.OnEntityCreated(e);

    uint32_t processed = 99;
    f.engine.Update(0, &processed);
    TESSERA_CHECK(processed == 1);
    TESSERA_CHECK(f.engine.Events(1).events.size() == 1);
    TESSERA_CHECK(f.engine.Events(1).events[0].kind == Kind::Enter);
    TESSERA_CHECK(f.engine.Events(1).events[0].id == e);

    f.engine.ClearEvents(1);
    f.engine.Update(1, &processed);
    TESSERA_CHECK(processed == 0); // nothing changed, so nothing is evaluated again
    TESSERA_CHECK(f.engine.Events(1).events.empty());
}

TESSERA_TEST(always_relevant_entities_get_compact_ids_in_raw_order) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    f.engine.OnEntityCreated(MakeId(kGlobalType, 5));
    f.engine.OnEntityCreated(MakeId(kGlobalType, 3));
    f.engine.OnEntityCreated(MakeId(kGlobalType, 9));
    f.engine.Update(0);
    const auto& events = f.engine.Events(0).events;
    TESSERA_CHECK(events.size() == 3);
    TESSERA_CHECK(events[0].id == MakeId(kGlobalType, 3) && events[0].compact_id == 0);
    TESSERA_CHECK(events[1].id == MakeId(kGlobalType, 5) && events[1].compact_id == 1);
    TESSERA_CHECK(events[2].id == MakeId(kGlobalType, 9) && events[2].compact_id == 2);
    TESSERA_CHECK(f.engine.CompactIdOf(0, MakeId(kGlobalType, 9)) == 2);
    TESSERA_CHECK(f.engine.VisibleCount(0) == 3);
}

TESSERA_TEST(owner_only_entity_is_visible_only_to_its_owner) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    f.engine.AddConnection(1);
    const EntityId e = MakeId(kOwnedType, 1);
    f.engine.OnEntityCreated(e);
    TESSERA_CHECK(f.engine.SetOwner(e, 1));
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.Events(0).events.empty());
    TESSERA_CHECK(f.engine.Events(1).events.size() == 1);
    TESSERA_CHECK(f.engine.CompactIdOf(1, e) != kInvalidCompactId);
    TESSERA_CHECK(f.engine.CompactIdOf(0, e) == kInvalidCompactId);
}

TESSERA_TEST(set_owner_rejects_unknown_entities_wrong_types_and_inactive_connections) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    const EntityId owned = MakeId(kOwnedType, 1);
    const EntityId spatial = MakeId(kSpatialType, 2);
    f.engine.OnEntityCreated(owned);
    f.engine.OnEntityCreated(spatial);
    TESSERA_CHECK(!f.engine.SetOwner(MakeId(kOwnedType, 77), 0)); // never created
    TESSERA_CHECK(!f.engine.SetOwner(spatial, 0));                // type is not owner-only
    TESSERA_CHECK(!f.engine.SetOwner(owned, 5));                  // connection not active
    TESSERA_CHECK(f.engine.SetOwner(owned, 0));
}

TESSERA_TEST(reassigning_the_owner_moves_visibility) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    f.engine.AddConnection(1);
    const EntityId e = MakeId(kOwnedType, 1);
    f.engine.OnEntityCreated(e);
    f.engine.SetOwner(e, 0);
    f.engine.Update(0);
    f.engine.ClearEvents(0);
    f.engine.ClearEvents(1);

    f.engine.SetOwner(e, 1);
    f.engine.Update(1);
    TESSERA_CHECK(f.engine.Events(0).events.size() == 1 && f.engine.Events(0).events[0].kind == Kind::Leave);
    TESSERA_CHECK(f.engine.Events(1).events.size() == 1 && f.engine.Events(1).events[0].kind == Kind::Enter);
}

TESSERA_TEST(clear_events_empties_the_list_and_events_accumulate_until_then) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    f.engine.OnEntityCreated(MakeId(kGlobalType, 1));
    f.engine.Update(0);
    f.engine.OnEntityCreated(MakeId(kGlobalType, 2));
    f.engine.Update(1);
    TESSERA_CHECK(f.engine.Events(0).events.size() == 2); // not cleared in between
    f.engine.ClearEvents(0);
    TESSERA_CHECK(f.engine.Events(0).events.empty());
}

TESSERA_TEST(compact_id_of_is_invalid_for_entities_that_are_not_visible) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    TESSERA_CHECK(f.engine.CompactIdOf(0, MakeId(kGlobalType, 1)) == kInvalidCompactId);
}

TESSERA_TEST(queries_on_unknown_connections_are_safe) {
    Fixture f(MakeConfig());
    TESSERA_CHECK(f.engine.Events(3).events.empty());
    TESSERA_CHECK(f.engine.VisibleCount(3) == 0);
    TESSERA_CHECK(f.engine.VisibleOverflow(3) == 0);
    TESSERA_CHECK(f.engine.CompactIdOf(3, MakeId(kGlobalType, 1)) == kInvalidCompactId);
    TESSERA_CHECK(f.engine.CompactIdOf(9999, MakeId(kGlobalType, 1)) == kInvalidCompactId);
    f.engine.ClearEvents(3); // no effect, no crash
}

// ---- Spatial mode, hysteresis and observers --------------------------------------------------------

namespace {

void Move(Fixture& f, EntityId id, float x, float y = 0.0f, float z = 0.0f) {
    f.grid.ApplyMoves({PendingMove{id, x, y, z}});
}

} // namespace

TESSERA_TEST(spatial_entity_enters_within_range_including_the_border) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    const Observer eye{0.0f, 0.0f, 0.0f};
    f.engine.SetObservers(0, std::span<const Observer>(&eye, 1));
    const EntityId on_border = MakeId(kSpatialType, 1);
    const EntityId just_outside = MakeId(kSpatialType, 2);
    f.grid.Insert(on_border, 100.0f, 0.0f, 0.0f);
    f.grid.Insert(just_outside, 100.01f, 0.0f, 0.0f);
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.CompactIdOf(0, on_border) != kInvalidCompactId);
    TESSERA_CHECK(f.engine.CompactIdOf(0, just_outside) == kInvalidCompactId);
}

TESSERA_TEST(visible_entity_stays_until_range_plus_margin_inclusive) {
    RelevancyConfig config = MakeConfig();
    config.types[kSpatialType].hysteresis_margin = 10.0f;
    Fixture f(config);
    f.engine.AddConnection(0);
    const Observer eye{0.0f, 0.0f, 0.0f};
    f.engine.SetObservers(0, std::span<const Observer>(&eye, 1));
    const EntityId e = MakeId(kSpatialType, 1);
    f.grid.Insert(e, 50.0f, 0.0f, 0.0f);
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.CompactIdOf(0, e) != kInvalidCompactId);

    Move(f, e, 110.0f);
    f.engine.Update(1);
    TESSERA_CHECK(f.engine.CompactIdOf(0, e) != kInvalidCompactId); // exactly range + margin

    Move(f, e, 110.01f);
    f.engine.Update(2);
    TESSERA_CHECK(f.engine.CompactIdOf(0, e) == kInvalidCompactId);
}

TESSERA_TEST(hidden_entity_inside_the_margin_does_not_enter) {
    RelevancyConfig config = MakeConfig();
    config.types[kSpatialType].hysteresis_margin = 10.0f;
    Fixture f(config);
    f.engine.AddConnection(0);
    const Observer eye{0.0f, 0.0f, 0.0f};
    f.engine.SetObservers(0, std::span<const Observer>(&eye, 1));
    const EntityId e = MakeId(kSpatialType, 1);
    f.grid.Insert(e, 105.0f, 0.0f, 0.0f);
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.CompactIdOf(0, e) == kInvalidCompactId);
}

TESSERA_TEST(entity_vibrating_at_the_border_enters_once_and_never_leaves) {
    RelevancyConfig config = MakeConfig();
    config.types[kSpatialType].hysteresis_margin = 10.0f;
    Fixture f(config);
    f.engine.AddConnection(0);
    const Observer eye{0.0f, 0.0f, 0.0f};
    f.engine.SetObservers(0, std::span<const Observer>(&eye, 1));
    const EntityId e = MakeId(kSpatialType, 1);
    f.grid.Insert(e, 95.0f, 0.0f, 0.0f);
    for (uint64_t tick = 0; tick < 30; ++tick) {
        Move(f, e, tick % 2 == 0 ? 95.0f : 105.0f);
        f.engine.Update(tick);
    }
    TESSERA_CHECK(CountEvents(f.engine.Events(0), Kind::Enter) == 1);
    TESSERA_CHECK(CountEvents(f.engine.Events(0), Kind::Leave) == 0);
}

TESSERA_TEST(ranges_are_per_type) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    const Observer eye{0.0f, 0.0f, 0.0f};
    f.engine.SetObservers(0, std::span<const Observer>(&eye, 1));
    const EntityId near_type = MakeId(kSpatialType, 1); // range 100
    const EntityId far_type = MakeId(kFarType, 2);      // range 500
    f.grid.Insert(near_type, 200.0f, 0.0f, 0.0f);
    f.grid.Insert(far_type, 200.0f, 0.0f, 0.0f);
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.CompactIdOf(0, near_type) == kInvalidCompactId);
    TESSERA_CHECK(f.engine.CompactIdOf(0, far_type) != kInvalidCompactId);
}

TESSERA_TEST(zero_range_type_is_never_visible_even_at_the_observer) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    const Observer eye{0.0f, 0.0f, 0.0f};
    f.engine.SetObservers(0, std::span<const Observer>(&eye, 1));
    const EntityId e = MakeId(kNoRangeType, 1);
    f.grid.Insert(e, 0.0f, 0.0f, 0.0f);
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.CompactIdOf(0, e) == kInvalidCompactId);
}

TESSERA_TEST(connection_without_observers_sees_global_and_owned_but_not_spatial) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    const EntityId spatial = MakeId(kSpatialType, 1);
    const EntityId global = MakeId(kGlobalType, 2);
    const EntityId owned = MakeId(kOwnedType, 3);
    f.grid.Insert(spatial, 0.0f, 0.0f, 0.0f);
    f.engine.OnEntityCreated(global);
    f.engine.OnEntityCreated(owned);
    f.engine.SetOwner(owned, 0);
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.CompactIdOf(0, spatial) == kInvalidCompactId);
    TESSERA_CHECK(f.engine.CompactIdOf(0, global) != kInvalidCompactId);
    TESSERA_CHECK(f.engine.CompactIdOf(0, owned) != kInvalidCompactId);
}

TESSERA_TEST(setting_an_empty_observer_list_makes_spatial_entities_leave) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    const Observer eye{0.0f, 0.0f, 0.0f};
    f.engine.SetObservers(0, std::span<const Observer>(&eye, 1));
    const EntityId e = MakeId(kSpatialType, 1);
    f.grid.Insert(e, 10.0f, 0.0f, 0.0f);
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.CompactIdOf(0, e) != kInvalidCompactId);

    TESSERA_CHECK(f.engine.SetObservers(0, {}));
    f.engine.Update(1);
    TESSERA_CHECK(f.engine.CompactIdOf(0, e) == kInvalidCompactId);
    TESSERA_CHECK(CountEvents(f.engine.Events(0), Kind::Leave) == 1);
}

TESSERA_TEST(observers_are_unioned) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    const Observer eyes[] = {{0.0f, 0.0f, 0.0f}, {1000.0f, 0.0f, 0.0f}};
    f.engine.SetObservers(0, eyes);
    const EntityId a = MakeId(kSpatialType, 1);
    const EntityId b = MakeId(kSpatialType, 2);
    const EntityId c = MakeId(kSpatialType, 3);
    f.grid.Insert(a, 10.0f, 0.0f, 0.0f);
    f.grid.Insert(b, 1010.0f, 0.0f, 0.0f);
    f.grid.Insert(c, 500.0f, 0.0f, 0.0f); // far from both
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.CompactIdOf(0, a) != kInvalidCompactId);
    TESSERA_CHECK(f.engine.CompactIdOf(0, b) != kInvalidCompactId);
    TESSERA_CHECK(f.engine.CompactIdOf(0, c) == kInvalidCompactId);
    TESSERA_CHECK(f.engine.VisibleCount(0) == 2);
}

TESSERA_TEST(entity_seen_by_two_observers_enters_once) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    const Observer eyes[] = {{0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}};
    f.engine.SetObservers(0, eyes);
    f.grid.Insert(MakeId(kSpatialType, 1), 5.0f, 0.0f, 0.0f);
    f.engine.Update(0);
    TESSERA_CHECK(CountEvents(f.engine.Events(0), Kind::Enter) == 1);
}

TESSERA_TEST(set_observers_rejects_bad_input_and_keeps_the_previous_list) {
    RelevancyConfig config = MakeConfig();
    config.max_observers_per_connection = 2;
    Fixture f(config);
    f.engine.AddConnection(0);
    const Observer eye{0.0f, 0.0f, 0.0f};
    TESSERA_CHECK(f.engine.SetObservers(0, std::span<const Observer>(&eye, 1)));

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const Observer bad_nan{nan, 0.0f, 0.0f};
    const Observer bad_inf{0.0f, inf, 0.0f};
    const Observer too_many[] = {eye, eye, eye};
    TESSERA_CHECK(!f.engine.SetObservers(0, std::span<const Observer>(&bad_nan, 1)));
    TESSERA_CHECK(!f.engine.SetObservers(0, std::span<const Observer>(&bad_inf, 1)));
    TESSERA_CHECK(!f.engine.SetObservers(0, too_many));
    TESSERA_CHECK(!f.engine.SetObservers(7, std::span<const Observer>(&eye, 1)));

    const EntityId e = MakeId(kSpatialType, 1);
    f.grid.Insert(e, 10.0f, 0.0f, 0.0f);
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.CompactIdOf(0, e) != kInvalidCompactId); // the first list still applies
}

TESSERA_TEST(spatial_pass_never_shows_owner_only_entities_to_other_connections) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    f.engine.AddConnection(1);
    const Observer eye{0.0f, 0.0f, 0.0f};
    f.engine.SetObservers(0, std::span<const Observer>(&eye, 1));
    const EntityId e = MakeId(kOwnedType, 1);
    f.engine.OnEntityCreated(e);
    f.engine.SetOwner(e, 1);
    f.grid.Insert(e, 0.0f, 0.0f, 0.0f); // standing right next to connection 0 observer
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.CompactIdOf(0, e) == kInvalidCompactId);
    TESSERA_CHECK(f.engine.CompactIdOf(1, e) != kInvalidCompactId);
}

TESSERA_TEST(global_entity_in_the_grid_enters_only_once) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    const Observer eye{0.0f, 0.0f, 0.0f};
    f.engine.SetObservers(0, std::span<const Observer>(&eye, 1));
    const EntityId e = MakeId(kGlobalType, 1);
    f.engine.OnEntityCreated(e);
    f.grid.Insert(e, 0.0f, 0.0f, 0.0f);
    f.engine.Update(0);
    f.engine.Update(1);
    TESSERA_CHECK(CountEvents(f.engine.Events(0), Kind::Enter) == 1);
}

TESSERA_TEST(entity_removed_from_the_grid_without_a_destroy_leaves_on_the_next_recalc) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    const Observer eye{0.0f, 0.0f, 0.0f};
    f.engine.SetObservers(0, std::span<const Observer>(&eye, 1));
    const EntityId e = MakeId(kSpatialType, 1);
    f.grid.Insert(e, 10.0f, 0.0f, 0.0f);
    f.engine.Update(0);
    f.grid.Remove(e);
    f.engine.Update(1);
    TESSERA_CHECK(f.engine.CompactIdOf(0, e) == kInvalidCompactId);
    TESSERA_CHECK(CountEvents(f.engine.Events(0), Kind::Leave) == 1);
}

// ---- Groups ----------------------------------------------------------------------------------------

namespace {

/// A connection with one observer at the origin.
void AddViewer(Fixture& f, ConnectionId id) {
    f.engine.AddConnection(id);
    const Observer eye{0.0f, 0.0f, 0.0f};
    f.engine.SetObservers(id, std::span<const Observer>(&eye, 1));
}

} // namespace

TESSERA_TEST(exclusion_hides_a_spatial_entity_and_makes_a_visible_one_leave) {
    Fixture f(MakeConfig());
    AddViewer(f, 0);
    const EntityId seen = MakeId(kSpatialType, 1);
    const EntityId hidden = MakeId(kSpatialType, 2);
    f.grid.Insert(seen, 10.0f, 0.0f, 0.0f);
    f.grid.Insert(hidden, 20.0f, 0.0f, 0.0f);
    const GroupId g = f.groups.CreateGroup(GroupKind::Exclusion);
    f.groups.AddMember(g, hidden);
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.CompactIdOf(0, seen) != kInvalidCompactId);
    TESSERA_CHECK(f.engine.CompactIdOf(0, hidden) == kInvalidCompactId);

    f.groups.AddMember(g, seen); // now an already visible entity is vetoed
    f.engine.Update(1);
    TESSERA_CHECK(f.engine.CompactIdOf(0, seen) == kInvalidCompactId);
    TESSERA_CHECK(CountEvents(f.engine.Events(0), Kind::Leave) == 1);
}

TESSERA_TEST(exclusion_hides_global_and_owned_entities) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    const EntityId global = MakeId(kGlobalType, 1);
    const EntityId owned = MakeId(kOwnedType, 2);
    f.engine.OnEntityCreated(global);
    f.engine.OnEntityCreated(owned);
    f.engine.SetOwner(owned, 0);
    const GroupId g = f.groups.CreateGroup(GroupKind::Exclusion);
    f.groups.AddMember(g, global);
    f.groups.AddMember(g, owned);
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.VisibleCount(0) == 0);
}

TESSERA_TEST(releasing_the_exclusion_for_one_connection_shows_the_entity_there_only) {
    Fixture f(MakeConfig());
    AddViewer(f, 0);
    AddViewer(f, 1);
    const EntityId e = MakeId(kSpatialType, 1);
    f.grid.Insert(e, 10.0f, 0.0f, 0.0f);
    const GroupId g = f.groups.CreateGroup(GroupKind::Exclusion);
    f.groups.AddMember(g, e);
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.VisibleCount(0) == 0 && f.engine.VisibleCount(1) == 0);

    f.groups.SetStatus(g, 1, true);
    f.engine.Update(1);
    TESSERA_CHECK(f.engine.CompactIdOf(1, e) != kInvalidCompactId);
    TESSERA_CHECK(f.engine.CompactIdOf(0, e) == kInvalidCompactId);
}

TESSERA_TEST(inclusion_shows_a_far_entity_to_the_allowed_connection_only) {
    Fixture f(MakeConfig());
    AddViewer(f, 0);
    AddViewer(f, 1);
    const EntityId far_away = MakeId(kSpatialType, 1);
    f.grid.Insert(far_away, 5000.0f, 0.0f, 0.0f);
    const GroupId g = f.groups.CreateGroup(GroupKind::Inclusion);
    f.groups.AddMember(g, far_away);
    f.groups.SetStatus(g, 1, true);
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.CompactIdOf(1, far_away) != kInvalidCompactId);
    TESSERA_CHECK(f.engine.CompactIdOf(0, far_away) == kInvalidCompactId);
}

TESSERA_TEST(exclusion_wins_over_inclusion_and_over_always_relevant) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    const EntityId e = MakeId(kGlobalType, 1);
    f.engine.OnEntityCreated(e);
    const GroupId include = f.groups.CreateGroup(GroupKind::Inclusion);
    const GroupId exclude = f.groups.CreateGroup(GroupKind::Exclusion);
    f.groups.AddMember(include, e);
    f.groups.SetStatusForAll(include, true);
    f.groups.AddMember(exclude, e);
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.CompactIdOf(0, e) == kInvalidCompactId);
}

TESSERA_TEST(entity_in_no_group_is_unaffected_by_other_groups) {
    Fixture f(MakeConfig());
    AddViewer(f, 0);
    const EntityId plain = MakeId(kSpatialType, 1);
    const EntityId vetoed = MakeId(kSpatialType, 2);
    f.grid.Insert(plain, 10.0f, 0.0f, 0.0f);
    f.grid.Insert(vetoed, 12.0f, 0.0f, 0.0f);
    const GroupId g = f.groups.CreateGroup(GroupKind::Exclusion);
    f.groups.AddMember(g, vetoed);
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.CompactIdOf(0, plain) != kInvalidCompactId);
}

TESSERA_TEST(destroying_a_group_restores_the_normal_rules) {
    Fixture f(MakeConfig());
    AddViewer(f, 0);
    const EntityId e = MakeId(kSpatialType, 1);
    f.grid.Insert(e, 10.0f, 0.0f, 0.0f);
    const GroupId g = f.groups.CreateGroup(GroupKind::Exclusion);
    f.groups.AddMember(g, e);
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.CompactIdOf(0, e) == kInvalidCompactId);
    f.groups.DestroyGroup(g);
    f.engine.Update(1);
    TESSERA_CHECK(f.engine.CompactIdOf(0, e) != kInvalidCompactId);
}

// ---- Destruction, connection removal and id overflow ----------------------------------------------

TESSERA_TEST(destroy_is_reported_without_a_full_reevaluation) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    const EntityId e = MakeId(kGlobalType, 1);
    f.engine.OnEntityCreated(e);
    f.engine.Update(0);
    const uint32_t compact = f.engine.CompactIdOf(0, e);
    TESSERA_CHECK(compact != kInvalidCompactId);
    f.engine.ClearEvents(0);

    f.engine.OnEntityDestroyed(e);
    uint32_t processed = 99;
    f.engine.Update(1, &processed);
    TESSERA_CHECK(processed == 0);
    TESSERA_CHECK(f.engine.Events(0).events.size() == 1);
    TESSERA_CHECK(f.engine.Events(0).events[0].kind == Kind::Destroy);
    TESSERA_CHECK(f.engine.Events(0).events[0].id == e);
    TESSERA_CHECK(f.engine.Events(0).events[0].compact_id == compact);
    TESSERA_CHECK(f.engine.VisibleCount(0) == 0);
}

TESSERA_TEST(destroying_an_unknown_id_does_nothing) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    const EntityId seen = MakeId(kGlobalType, 1);
    f.engine.OnEntityCreated(seen);
    f.engine.Update(0);
    f.engine.ClearEvents(0);

    f.engine.OnEntityDestroyed(MakeId(kSpatialType, 99)); // never existed
    f.engine.OnEntityDestroyed(MakeId(kGlobalType, 2));   // a type the engine tracks, but never created
    f.engine.Update(1);
    TESSERA_CHECK(f.engine.Events(0).events.empty());
    TESSERA_CHECK(f.engine.CompactIdOf(0, seen) != kInvalidCompactId); // an unrelated entity is untouched
    TESSERA_CHECK(f.engine.VisibleCount(0) == 1);
}

TESSERA_TEST(events_in_one_update_come_as_destroy_then_leave_then_enter_each_in_id_order) {
    Fixture f(MakeConfig());
    AddViewer(f, 0);
    constexpr uint32_t kEach = 8;
    // Everything is created, and made visible, in descending index order, one entity per tick for the
    // group that will leave. Containers that remember insertion order then hold the entities in
    // descending order, so ascending output can only come from the engine sorting it.
    for (uint32_t i = kEach; i-- > 0;) {
        f.grid.Insert(MakeId(kSpatialType, 100 + i), 10.0f, 0.0f, 0.0f);   // will be destroyed
        f.grid.Insert(MakeId(kSpatialType, 200 + i), 5000.0f, 0.0f, 0.0f); // will enter, then leave
        f.grid.Insert(MakeId(kSpatialType, 300 + i), 5000.0f, 0.0f, 0.0f); // will enter at the end
    }
    uint64_t tick = 0;
    f.engine.Update(tick++);
    for (uint32_t i = kEach; i-- > 0;) {
        Move(f, MakeId(kSpatialType, 200 + i), 20.0f);
        f.engine.Update(tick++);
    }
    f.engine.ClearEvents(0);

    for (uint32_t i = kEach; i-- > 0;) {
        f.grid.Remove(MakeId(kSpatialType, 100 + i));
        f.engine.OnEntityDestroyed(MakeId(kSpatialType, 100 + i));
        Move(f, MakeId(kSpatialType, 200 + i), 5000.0f);
        Move(f, MakeId(kSpatialType, 300 + i), 30.0f);
    }
    f.engine.Update(tick);

    const auto& events = f.engine.Events(0).events;
    TESSERA_CHECK(events.size() == 3 * kEach);
    for (uint32_t i = 0; i < kEach && events.size() == 3 * kEach; ++i) {
        TESSERA_CHECK(events[i].kind == Kind::Destroy && events[i].id == MakeId(kSpatialType, 100 + i));
        TESSERA_CHECK(events[kEach + i].kind == Kind::Leave &&
                      events[kEach + i].id == MakeId(kSpatialType, 200 + i));
        TESSERA_CHECK(events[2 * kEach + i].kind == Kind::Enter &&
                      events[2 * kEach + i].id == MakeId(kSpatialType, 300 + i));
    }
}

TESSERA_TEST(destroying_the_same_id_twice_emits_one_destroy) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    const EntityId e = MakeId(kGlobalType, 1);
    f.engine.OnEntityCreated(e);
    f.engine.Update(0);
    f.engine.ClearEvents(0);
    f.engine.OnEntityDestroyed(e);
    f.engine.OnEntityDestroyed(e);
    f.engine.Update(1);
    f.engine.OnEntityDestroyed(e); // again, in a later tick
    f.engine.Update(2);
    TESSERA_CHECK(CountEvents(f.engine.Events(0), Kind::Destroy) == 1);
}

TESSERA_TEST(destroyed_entity_leaves_groups_and_is_not_reentered_by_stale_inclusion) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    const EntityId e = MakeId(kSpatialType, 1);
    f.grid.Insert(e, 5000.0f, 0.0f, 0.0f);
    const GroupId g = f.groups.CreateGroup(GroupKind::Inclusion);
    f.groups.AddMember(g, e);
    f.groups.SetStatusForAll(g, true);
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.CompactIdOf(0, e) != kInvalidCompactId);
    f.engine.ClearEvents(0);

    f.grid.Remove(e);
    f.engine.OnEntityDestroyed(e);
    TESSERA_CHECK(!f.groups.IsMember(g, e));
    f.engine.Update(1);
    f.engine.Update(2);
    TESSERA_CHECK(CountEvents(f.engine.Events(0), Kind::Destroy) == 1);
    TESSERA_CHECK(CountEvents(f.engine.Events(0), Kind::Enter) == 0);
    TESSERA_CHECK(f.engine.VisibleCount(0) == 0);
}

TESSERA_TEST(destroyed_global_entity_is_not_shown_to_later_connections) {
    Fixture f(MakeConfig());
    const EntityId e = MakeId(kGlobalType, 1);
    f.engine.OnEntityCreated(e);
    f.engine.OnEntityDestroyed(e);
    f.engine.Update(0);
    f.engine.AddConnection(0);
    f.engine.Update(1);
    TESSERA_CHECK(f.engine.VisibleCount(0) == 0);
}

TESSERA_TEST(reused_connection_id_starts_clean) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    const EntityId e = MakeId(kGlobalType, 1);
    f.engine.OnEntityCreated(e);
    const GroupId g = f.groups.CreateGroup(GroupKind::Exclusion);
    f.groups.SetStatus(g, 0, true);
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.VisibleCount(0) == 1);

    TESSERA_CHECK(f.engine.RemoveConnection(0));
    TESSERA_CHECK(!f.groups.IsAllowed(g, 0)); // the old exception is gone
    TESSERA_CHECK(f.engine.AddConnection(0));
    TESSERA_CHECK(f.engine.VisibleCount(0) == 0);
    TESSERA_CHECK(f.engine.Events(0).events.empty());
    f.engine.Update(1);
    TESSERA_CHECK(f.engine.Events(0).events.size() == 1);
    TESSERA_CHECK(f.engine.Events(0).events[0].compact_id == 0); // compact ids start over
}

TESSERA_TEST(entities_of_a_removed_owner_become_ownerless_and_can_be_given_again) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    const EntityId e = MakeId(kOwnedType, 1);
    f.engine.OnEntityCreated(e);
    f.engine.SetOwner(e, 0);
    f.engine.RemoveConnection(0);
    f.engine.AddConnection(0);
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.VisibleCount(0) == 0); // the new connection 0 does not inherit it
    TESSERA_CHECK(f.engine.SetOwner(e, 0));
    f.engine.Update(1);
    TESSERA_CHECK(f.engine.CompactIdOf(0, e) != kInvalidCompactId);
}

TESSERA_TEST(overflow_keeps_extra_entities_out_and_counts_them) {
    RelevancyConfig config = MakeConfig();
    config.max_visible_per_connection = 2;
    Fixture f(config);
    f.engine.AddConnection(0);
    for (uint32_t i = 1; i <= 3; ++i) {
        f.engine.OnEntityCreated(MakeId(kGlobalType, i));
    }
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.VisibleCount(0) == 2);
    TESSERA_CHECK(f.engine.VisibleOverflow(0) == 1);
    TESSERA_CHECK(f.engine.CompactIdOf(0, MakeId(kGlobalType, 3)) == kInvalidCompactId);
}

TESSERA_TEST(overflowed_entity_enters_once_an_id_is_free_for_a_full_tick) {
    RelevancyConfig config = MakeConfig();
    config.max_visible_per_connection = 2;
    Fixture f(config);
    f.engine.AddConnection(0);
    for (uint32_t i = 1; i <= 3; ++i) {
        f.engine.OnEntityCreated(MakeId(kGlobalType, i));
    }
    f.engine.Update(0);
    f.engine.OnEntityDestroyed(MakeId(kGlobalType, 1));
    f.engine.Update(1);
    // The freed id is not reusable in the tick it was freed.
    TESSERA_CHECK(f.engine.CompactIdOf(0, MakeId(kGlobalType, 3)) == kInvalidCompactId);
    f.engine.Update(2);
    TESSERA_CHECK(f.engine.CompactIdOf(0, MakeId(kGlobalType, 3)) != kInvalidCompactId);
}

TESSERA_TEST(enter_in_the_tick_of_a_leave_does_not_reuse_the_freed_id) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    const Observer eye{0.0f, 0.0f, 0.0f};
    f.engine.SetObservers(0, std::span<const Observer>(&eye, 1));
    const EntityId leaving = MakeId(kSpatialType, 1);
    const EntityId entering = MakeId(kSpatialType, 2);
    f.grid.Insert(leaving, 10.0f, 0.0f, 0.0f);
    f.grid.Insert(entering, 5000.0f, 0.0f, 0.0f);
    f.engine.Update(0);
    const uint32_t freed = f.engine.CompactIdOf(0, leaving);
    TESSERA_CHECK(freed != kInvalidCompactId);

    f.grid.ApplyMoves({PendingMove{leaving, 5000.0f, 0.0f, 0.0f}, PendingMove{entering, 10.0f, 0.0f, 0.0f}});
    f.engine.Update(1);
    const uint32_t taken = f.engine.CompactIdOf(0, entering);
    TESSERA_CHECK(taken != kInvalidCompactId);
    TESSERA_CHECK(taken != freed);
}

// ---- Anchors, tolerance and the re-anchor cap ----------------------------------------------------------

namespace {

bool Sees(const Fixture& f, ConnectionId connection, EntityId entity) {
    return f.engine.CompactIdOf(connection, entity) != kInvalidCompactId;
}

void SetOneObserver(Fixture& f, ConnectionId connection, float x, float y = 0.0f, float z = 0.0f) {
    const Observer eye{x, y, z};
    f.engine.SetObservers(connection, std::span<const Observer>(&eye, 1));
}

} // namespace

TESSERA_TEST(observer_moving_less_than_the_tolerance_keeps_its_anchor) {
    Fixture f(MakeConfig()); // tolerance 4 m, spatial range 100 m
    f.engine.AddConnection(0);
    SetOneObserver(f, 0, 0.0f);
    const EntityId e = MakeId(kSpatialType, 1);
    f.grid.Insert(e, 103.0f, 0.0f, 0.0f);
    f.engine.Update(0);
    TESSERA_CHECK(!Sees(f, 0, e));

    uint32_t processed = 99;
    SetOneObserver(f, 0, 3.9f); // the true distance is 99.1 m, but the anchor has not moved
    f.engine.Update(1, &processed);
    TESSERA_CHECK(processed == 0);
    TESSERA_CHECK(!Sees(f, 0, e));

    SetOneObserver(f, 0, 4.0f); // exactly the tolerance: evaluate again
    f.engine.Update(2, &processed);
    TESSERA_CHECK(processed == 1);
    TESSERA_CHECK(Sees(f, 0, e));
}

TESSERA_TEST(changing_the_number_of_observers_reanchors) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    SetOneObserver(f, 0, 0.0f);
    const EntityId near_second = MakeId(kSpatialType, 1);
    f.grid.Insert(near_second, 5010.0f, 0.0f, 0.0f);
    f.engine.Update(0);
    TESSERA_CHECK(!Sees(f, 0, near_second));

    const Observer eyes[] = {{0.0f, 0.0f, 0.0f}, {5000.0f, 0.0f, 0.0f}};
    f.engine.SetObservers(0, eyes);
    uint32_t processed = 99;
    f.engine.Update(1, &processed);
    TESSERA_CHECK(processed == 1);
    TESSERA_CHECK(Sees(f, 0, near_second));
}

TESSERA_TEST(teleporting_observer_reanchors_in_the_same_update) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    SetOneObserver(f, 0, 0.0f);
    const EntityId far_entity = MakeId(kSpatialType, 1);
    f.grid.Insert(far_entity, 5010.0f, 0.0f, 0.0f);
    f.engine.Update(0);
    TESSERA_CHECK(!Sees(f, 0, far_entity));

    SetOneObserver(f, 0, 5000.0f);
    uint32_t processed = 99;
    f.engine.Update(1, &processed);
    TESSERA_CHECK(processed == 1);
    TESSERA_CHECK(Sees(f, 0, far_entity));
}

TESSERA_TEST(budget_serves_the_oldest_first_and_reports_deferred) {
    RelevancyConfig config = MakeConfig();
    config.max_reanchors_per_tick = 2;
    Fixture f(config);
    const EntityId e = MakeId(kSpatialType, 1);
    f.grid.Insert(e, 10.0f, 0.0f, 0.0f);
    for (ConnectionId c = 0; c < 5; ++c) {
        f.engine.AddConnection(c);
        SetOneObserver(f, c, 0.0f);
    }
    uint32_t processed = 99;
    f.engine.Update(0, &processed);
    TESSERA_CHECK(processed == 2 && f.engine.DeferredReanchors() == 3);
    TESSERA_CHECK(Sees(f, 0, e) && Sees(f, 1, e) && !Sees(f, 2, e) && !Sees(f, 3, e) && !Sees(f, 4, e));

    f.engine.Update(1, &processed);
    TESSERA_CHECK(processed == 2 && f.engine.DeferredReanchors() == 1);
    TESSERA_CHECK(Sees(f, 2, e) && Sees(f, 3, e) && !Sees(f, 4, e));

    f.engine.Update(2, &processed);
    TESSERA_CHECK(processed == 1 && f.engine.DeferredReanchors() == 0);
    TESSERA_CHECK(Sees(f, 4, e));
}

TESSERA_TEST(new_connection_waits_behind_older_pending_ones) {
    RelevancyConfig config = MakeConfig();
    config.max_reanchors_per_tick = 1;
    Fixture f(config);
    const EntityId e = MakeId(kSpatialType, 1);
    f.grid.Insert(e, 10.0f, 0.0f, 0.0f);
    for (ConnectionId c = 0; c < 2; ++c) {
        f.engine.AddConnection(c);
        SetOneObserver(f, c, 0.0f);
    }
    f.engine.Update(0);
    TESSERA_CHECK(Sees(f, 0, e) && !Sees(f, 1, e));

    f.engine.AddConnection(2); // arrives while connection 1 is still waiting
    SetOneObserver(f, 2, 0.0f);
    f.engine.Update(1);
    TESSERA_CHECK(Sees(f, 1, e) && !Sees(f, 2, e));

    f.engine.Update(2);
    TESSERA_CHECK(Sees(f, 2, e));
}

TESSERA_TEST(zero_cap_means_no_limit) {
    RelevancyConfig config = MakeConfig();
    config.max_reanchors_per_tick = 0;
    Fixture f(config);
    const EntityId e = MakeId(kSpatialType, 1);
    f.grid.Insert(e, 10.0f, 0.0f, 0.0f);
    for (ConnectionId c = 0; c < 5; ++c) {
        f.engine.AddConnection(c);
        SetOneObserver(f, c, 0.0f);
    }
    uint32_t processed = 0;
    f.engine.Update(0, &processed);
    TESSERA_CHECK(processed == 5 && f.engine.DeferredReanchors() == 0);
    for (ConnectionId c = 0; c < 5; ++c) {
        TESSERA_CHECK(Sees(f, c, e));
    }
}

TESSERA_TEST(owner_group_and_always_relevant_changes_mark_full_reevaluation) {
    Fixture f(MakeConfig());
    for (ConnectionId c = 0; c < 3; ++c) {
        f.engine.AddConnection(c);
    }
    uint32_t processed = 99;
    f.engine.Update(0, &processed); // the first evaluation of each new connection
    TESSERA_CHECK(processed == 3);
    f.engine.Update(1, &processed);
    TESSERA_CHECK(processed == 0);

    const EntityId owned = MakeId(kOwnedType, 1);
    f.engine.OnEntityCreated(owned); // nobody owns it yet, so nobody can see it
    f.engine.Update(2, &processed);
    TESSERA_CHECK(processed == 0);

    f.engine.SetOwner(owned, 0);
    f.engine.Update(3, &processed);
    TESSERA_CHECK(processed == 1);

    f.engine.SetOwner(owned, 1); // the old owner and the new one
    f.engine.Update(4, &processed);
    TESSERA_CHECK(processed == 2);

    f.groups.CreateGroup(GroupKind::Exclusion); // any change to the groups
    f.engine.Update(5, &processed);
    TESSERA_CHECK(processed == 3);

    f.engine.OnEntityCreated(MakeId(kGlobalType, 2)); // visible to everybody
    f.engine.Update(6, &processed);
    TESSERA_CHECK(processed == 3);
}

TESSERA_TEST(destroying_a_grouped_entity_does_not_mark_other_connections) {
    Fixture f(MakeConfig());
    for (ConnectionId c = 0; c < 3; ++c) {
        f.engine.AddConnection(c);
    }
    const EntityId e = MakeId(kGlobalType, 1);
    f.engine.OnEntityCreated(e);
    const GroupId g = f.groups.CreateGroup(GroupKind::Inclusion);
    f.groups.AddMember(g, e);
    uint32_t processed = 99;
    f.engine.Update(0, &processed); // evaluates everything and sees the current group version
    f.engine.Update(1, &processed);
    TESSERA_CHECK(processed == 0);

    f.engine.OnEntityDestroyed(e); // takes the entity out of the group, which changes the version
    f.engine.Update(2, &processed);
    TESSERA_CHECK(processed == 0);
    TESSERA_CHECK(CountEvents(f.engine.Events(0), Kind::Destroy) == 1);
}

TESSERA_TEST(a_game_change_to_the_groups_is_not_hidden_by_an_engine_removal) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    const EntityId doomed = MakeId(kGlobalType, 1);
    const EntityId other = MakeId(kGlobalType, 2);
    f.engine.OnEntityCreated(doomed);
    f.engine.OnEntityCreated(other);
    const GroupId g = f.groups.CreateGroup(GroupKind::Inclusion);
    f.groups.AddMember(g, doomed);
    f.engine.Update(0);
    f.engine.ClearEvents(0);

    const GroupId hide = f.groups.CreateGroup(GroupKind::Exclusion); // a game change, not yet seen
    f.groups.AddMember(hide, other);
    f.engine.OnEntityDestroyed(doomed); // the engine's own change must not hide the game's
    uint32_t processed = 99;
    f.engine.Update(1, &processed);
    TESSERA_CHECK(processed == 1);
    TESSERA_CHECK(!Sees(f, 0, other));
}

TESSERA_TEST(new_engine_starts_with_an_empty_feed_and_turns_recording_on) {
    SpatialGrid grid{GridConfig{64.0f, 64.0f, 64.0f}};
    RelevancyGroups groups;
    ThreadPoolJobSystem jobs(1);
    grid.SetRecordChanges(true);
    grid.Insert(MakeId(kSpatialType, 1), 1.0f, 2.0f, 3.0f); // before the engine exists
    RelevancyEngine engine(MakeConfig(), grid, groups, jobs);
    TESSERA_CHECK(grid.Changes().empty());
    grid.Insert(MakeId(kSpatialType, 2), 4.0f, 5.0f, 6.0f);
    TESSERA_CHECK(grid.Changes().size() == 1); // the engine switched recording on
}

TESSERA_TEST(engine_clears_the_feed_after_each_update_and_stops_recording_when_destroyed) {
    SpatialGrid grid{GridConfig{64.0f, 64.0f, 64.0f}};
    RelevancyGroups groups;
    ThreadPoolJobSystem jobs(1);
    {
        RelevancyEngine engine(MakeConfig(), grid, groups, jobs);
        engine.AddConnection(0);
        grid.Insert(MakeId(kSpatialType, 1), 1.0f, 2.0f, 3.0f);
        TESSERA_CHECK(grid.Changes().size() == 1);
        engine.Update(0);
        TESSERA_CHECK(grid.Changes().empty());
    }
    grid.Insert(MakeId(kSpatialType, 2), 4.0f, 5.0f, 6.0f);
    TESSERA_CHECK(grid.Changes().empty()); // nobody is recording any more
}

// ---- Evaluating grid changes one by one -------------------------------------------------------------

namespace {

/// A connection with one observer at the origin that has been evaluated, with its events cleared.
void SettleViewer(Fixture& f, uint64_t& tick) {
    AddViewer(f, 0);
    f.engine.Update(tick++);
    f.engine.ClearEvents(0);
}

} // namespace

TESSERA_TEST(far_movement_causes_no_full_evaluation) {
    Fixture f(MakeConfig());
    const EntityId far_one = MakeId(kSpatialType, 1);
    f.grid.Insert(far_one, 5000.0f, 0.0f, 0.0f);
    uint64_t tick = 0;
    SettleViewer(f, tick);
    Move(f, far_one, 5010.0f);
    uint32_t processed = 99;
    f.engine.Update(tick, &processed);
    TESSERA_CHECK(processed == 0);
    TESSERA_CHECK(f.engine.Events(0).events.empty());
    TESSERA_CHECK(!Sees(f, 0, far_one));
}

TESSERA_TEST(new_spatial_entity_enters_without_a_full_evaluation) {
    Fixture f(MakeConfig());
    uint64_t tick = 0;
    SettleViewer(f, tick);
    const EntityId e = MakeId(kSpatialType, 1);
    f.grid.Insert(e, 10.0f, 0.0f, 0.0f);
    uint32_t processed = 99;
    f.engine.Update(tick, &processed);
    TESSERA_CHECK(processed == 0);
    TESSERA_CHECK(Sees(f, 0, e));
    TESSERA_CHECK(CountEvents(f.engine.Events(0), Kind::Enter) == 1);
}

TESSERA_TEST(entity_moving_into_range_enters_in_the_same_update_including_the_border) {
    Fixture f(MakeConfig()); // range 100
    const EntityId on_border = MakeId(kSpatialType, 1);
    const EntityId just_outside = MakeId(kSpatialType, 2);
    f.grid.Insert(on_border, 150.0f, 0.0f, 0.0f);
    f.grid.Insert(just_outside, 150.0f, 0.0f, 0.0f);
    uint64_t tick = 0;
    SettleViewer(f, tick);
    Move(f, on_border, 100.0f);
    Move(f, just_outside, 100.01f);
    uint32_t processed = 99;
    f.engine.Update(tick, &processed);
    TESSERA_CHECK(processed == 0);
    TESSERA_CHECK(Sees(f, 0, on_border));
    TESSERA_CHECK(!Sees(f, 0, just_outside));
}

TESSERA_TEST(entity_leaves_only_beyond_range_plus_margin_inclusive) {
    RelevancyConfig config = MakeConfig();
    config.types[kSpatialType].hysteresis_margin = 10.0f;
    Fixture f(config);
    const EntityId e = MakeId(kSpatialType, 1);
    f.grid.Insert(e, 50.0f, 0.0f, 0.0f);
    uint64_t tick = 0;
    SettleViewer(f, tick);
    TESSERA_CHECK(Sees(f, 0, e));

    Move(f, e, 110.0f);
    uint32_t processed = 99;
    f.engine.Update(tick++, &processed);
    TESSERA_CHECK(processed == 0 && Sees(f, 0, e) && f.engine.Events(0).events.empty());

    Move(f, e, 110.01f);
    f.engine.Update(tick++, &processed);
    TESSERA_CHECK(processed == 0 && !Sees(f, 0, e));
    TESSERA_CHECK(CountEvents(f.engine.Events(0), Kind::Leave) == 1);
}

TESSERA_TEST(entity_inserted_and_removed_in_one_tick_makes_no_event) {
    Fixture f(MakeConfig());
    uint64_t tick = 0;
    SettleViewer(f, tick);
    const EntityId e = MakeId(kSpatialType, 1);
    f.grid.Insert(e, 10.0f, 0.0f, 0.0f);
    f.grid.Remove(e);
    f.engine.Update(tick);
    TESSERA_CHECK(f.engine.Events(0).events.empty());
    TESSERA_CHECK(!Sees(f, 0, e));
}

TESSERA_TEST(entity_removed_and_inserted_again_in_one_tick_keeps_its_compact_id) {
    Fixture f(MakeConfig());
    const EntityId e = MakeId(kSpatialType, 1);
    f.grid.Insert(e, 10.0f, 0.0f, 0.0f);
    uint64_t tick = 0;
    SettleViewer(f, tick);
    const uint32_t compact = f.engine.CompactIdOf(0, e);
    TESSERA_CHECK(compact != kInvalidCompactId);
    f.grid.Remove(e);
    f.grid.Insert(e, 10.0f, 0.0f, 0.0f);
    f.engine.Update(tick);
    TESSERA_CHECK(f.engine.Events(0).events.empty());
    TESSERA_CHECK(f.engine.CompactIdOf(0, e) == compact);
}

TESSERA_TEST(entity_removed_from_the_grid_leaves_in_that_update) {
    Fixture f(MakeConfig());
    const EntityId e = MakeId(kSpatialType, 1);
    f.grid.Insert(e, 10.0f, 0.0f, 0.0f);
    uint64_t tick = 0;
    SettleViewer(f, tick);
    f.grid.Remove(e); // no OnEntityDestroyed
    uint32_t processed = 99;
    f.engine.Update(tick, &processed);
    TESSERA_CHECK(processed == 0);
    TESSERA_CHECK(!Sees(f, 0, e));
    TESSERA_CHECK(CountEvents(f.engine.Events(0), Kind::Leave) == 1);
}

TESSERA_TEST(moving_entity_included_by_a_group_stays_visible_after_leaving_range) {
    Fixture f(MakeConfig());
    const EntityId e = MakeId(kSpatialType, 1);
    f.grid.Insert(e, 10.0f, 0.0f, 0.0f);
    const GroupId g = f.groups.CreateGroup(GroupKind::Inclusion);
    f.groups.AddMember(g, e);
    f.groups.SetStatusForAll(g, true);
    uint64_t tick = 0;
    SettleViewer(f, tick);
    TESSERA_CHECK(Sees(f, 0, e));
    Move(f, e, 5000.0f);
    uint32_t processed = 99;
    f.engine.Update(tick, &processed);
    TESSERA_CHECK(processed == 0);
    TESSERA_CHECK(Sees(f, 0, e));
    TESSERA_CHECK(f.engine.Events(0).events.empty());
}

TESSERA_TEST(excluded_entity_moving_into_range_stays_hidden) {
    Fixture f(MakeConfig());
    const EntityId e = MakeId(kSpatialType, 1);
    f.grid.Insert(e, 150.0f, 0.0f, 0.0f);
    const GroupId g = f.groups.CreateGroup(GroupKind::Exclusion);
    f.groups.AddMember(g, e);
    uint64_t tick = 0;
    SettleViewer(f, tick);
    Move(f, e, 50.0f);
    uint32_t processed = 99;
    f.engine.Update(tick, &processed);
    TESSERA_CHECK(processed == 0);
    TESSERA_CHECK(!Sees(f, 0, e));
    TESSERA_CHECK(f.engine.Events(0).events.empty());
}

TESSERA_TEST(global_and_owner_types_in_the_feed_are_ignored) {
    Fixture f(MakeConfig());
    const EntityId global = MakeId(kGlobalType, 1);
    const EntityId owned = MakeId(kOwnedType, 2);
    f.engine.OnEntityCreated(global);
    f.engine.OnEntityCreated(owned);
    f.grid.Insert(global, 10.0f, 0.0f, 0.0f);
    f.grid.Insert(owned, 20.0f, 0.0f, 0.0f);
    uint64_t tick = 0;
    SettleViewer(f, tick);
    TESSERA_CHECK(Sees(f, 0, global) && !Sees(f, 0, owned));
    Move(f, global, 9000.0f);
    Move(f, owned, 30.0f);
    uint32_t processed = 99;
    f.engine.Update(tick, &processed);
    TESSERA_CHECK(processed == 0);
    TESSERA_CHECK(Sees(f, 0, global) && !Sees(f, 0, owned));
    TESSERA_CHECK(f.engine.Events(0).events.empty());
}

TESSERA_TEST(feed_with_two_observers_uses_the_nearest_anchor) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    const Observer eyes[] = {{0.0f, 0.0f, 0.0f}, {1000.0f, 0.0f, 0.0f}};
    f.engine.SetObservers(0, eyes);
    const EntityId e = MakeId(kSpatialType, 1);
    f.grid.Insert(e, 50.0f, 0.0f, 0.0f);
    uint64_t tick = 0;
    f.engine.Update(tick++);
    f.engine.ClearEvents(0);
    TESSERA_CHECK(Sees(f, 0, e));

    Move(f, e, 1050.0f); // leaves the first observer's range, enters the second one's
    uint32_t processed = 99;
    f.engine.Update(tick++, &processed);
    TESSERA_CHECK(processed == 0 && Sees(f, 0, e) && f.engine.Events(0).events.empty());

    Move(f, e, 500.0f); // far from both
    f.engine.Update(tick++, &processed);
    TESSERA_CHECK(processed == 0 && !Sees(f, 0, e));
    TESSERA_CHECK(CountEvents(f.engine.Events(0), Kind::Leave) == 1);
}

TESSERA_TEST(a_change_near_one_connection_leaves_the_others_untouched) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    f.engine.AddConnection(1);
    SetOneObserver(f, 0, 0.0f);
    SetOneObserver(f, 1, 5000.0f);
    const EntityId e = MakeId(kSpatialType, 1);
    f.grid.Insert(e, 4000.0f, 0.0f, 0.0f);
    f.engine.Update(0);
    f.engine.ClearEvents(0);
    f.engine.ClearEvents(1);

    Move(f, e, 10.0f); // now inside the first connection's range
    f.engine.Update(1);
    TESSERA_CHECK(Sees(f, 0, e) && !Sees(f, 1, e));
    TESSERA_CHECK(CountEvents(f.engine.Events(0), Kind::Enter) == 1);
    TESSERA_CHECK(f.engine.Events(1).events.empty());
}

TESSERA_TEST(one_event_per_entity_per_tick) {
    Fixture f(MakeConfig());
    const EntityId e = MakeId(kSpatialType, 1);
    f.grid.Insert(e, 5000.0f, 0.0f, 0.0f);
    uint64_t tick = 0;
    SettleViewer(f, tick);
    SetOneObserver(f, 0, 5000.0f); // a full evaluation (the observer jumped) ...
    Move(f, e, 5005.0f);           // ... and a change to the same entity in the same tick
    f.engine.Update(tick);
    TESSERA_CHECK(Sees(f, 0, e));
    TESSERA_CHECK(f.engine.Events(0).events.size() == 1);
    TESSERA_CHECK(f.engine.Events(0).events[0].kind == Kind::Enter);
}

TESSERA_TEST(compact_ids_stay_distinct_when_many_entities_enter_through_the_feed) {
    Fixture f(MakeConfig());
    uint64_t tick = 0;
    SettleViewer(f, tick);
    for (uint32_t i = 0; i < 200; ++i) {
        f.grid.Insert(MakeId(kSpatialType, 100 + i), static_cast<float>(i % 90), 0.0f, 0.0f);
    }
    f.engine.Update(tick);
    std::set<uint32_t> ids;
    for (const VisibilityEvent& event : f.engine.Events(0).events) {
        TESSERA_CHECK(event.kind == Kind::Enter);
        TESSERA_CHECK(ids.insert(event.compact_id).second);
    }
    TESSERA_CHECK(ids.size() == 200);
    TESSERA_CHECK(f.engine.VisibleCount(0) == 200);
}

TESSERA_TEST(entity_moved_twice_in_one_tick_is_judged_from_its_first_and_last_position) {
    Fixture f(MakeConfig());
    const EntityId leaving = MakeId(kSpatialType, 1);
    const EntityId arriving = MakeId(kSpatialType, 2);
    f.grid.Insert(leaving, 10.0f, 0.0f, 0.0f);
    f.grid.Insert(arriving, 4000.0f, 0.0f, 0.0f);
    uint64_t tick = 0;
    SettleViewer(f, tick);
    TESSERA_CHECK(Sees(f, 0, leaving) && !Sees(f, 0, arriving));

    // Both go through a position on the other side of the world before reaching the final one.
    f.grid.ApplyMoves({PendingMove{leaving, 5000.0f, 0.0f, 0.0f}, PendingMove{arriving, 3000.0f, 0.0f, 0.0f},
                       PendingMove{leaving, 5100.0f, 0.0f, 0.0f}, PendingMove{arriving, 20.0f, 0.0f, 0.0f}});
    uint32_t processed = 99;
    f.engine.Update(tick, &processed);
    TESSERA_CHECK(processed == 0);
    TESSERA_CHECK(!Sees(f, 0, leaving));
    TESSERA_CHECK(Sees(f, 0, arriving));
    TESSERA_CHECK(f.engine.Events(0).events.size() == 2);
}

// ---- Guards for finding the nearby changes quickly --------------------------------------------------

TESSERA_TEST(feed_finds_changes_on_both_sides_of_the_index_cell_boundaries) {
    // The engine looks changes up in a coarse grid whose cells are as large as the widest reach (500 m
    // here). Anchors and entities straddle the cell edges and the distance limit.
    const float anchors[] = {-250.0f, -0.01f, 0.0f, 0.01f, 250.0f, 499.99f, 500.0f, 500.01f, 750.0f, 1000.0f};
    const float offsets[] = {-500.01f, -500.0f, -499.99f, 499.99f, 500.0f, 500.01f};
    for (const float anchor : anchors) {
        for (const float offset : offsets) {
            Fixture f(MakeConfig());
            f.engine.AddConnection(0);
            SetOneObserver(f, 0, anchor);
            f.engine.Update(0);
            f.engine.ClearEvents(0);
            const float x = anchor + offset;
            const EntityId e = MakeId(kFarType, 1);
            f.grid.Insert(e, x, 0.0f, 0.0f);
            f.engine.Update(1);
            const bool expected = std::fabs(static_cast<double>(x) - static_cast<double>(anchor)) <= 500.0;
            TESSERA_CHECK(Sees(f, 0, e) == expected);
        }
    }
}

TESSERA_TEST(entity_moving_across_an_index_cell_boundary_is_followed) {
    Fixture f(MakeConfig());
    f.engine.AddConnection(0);
    SetOneObserver(f, 0, 0.0f);
    const EntityId e = MakeId(kFarType, 1);
    f.grid.Insert(e, 600.0f, 0.0f, 0.0f); // outside the 500 m range, in the next index cell
    f.engine.Update(0);
    f.engine.ClearEvents(0);
    TESSERA_CHECK(!Sees(f, 0, e));
    Move(f, e, 499.0f); // crosses the cell edge at 500 and enters
    f.engine.Update(1);
    TESSERA_CHECK(Sees(f, 0, e));
    f.engine.ClearEvents(0);
    Move(f, e, 501.0f); // crosses back and leaves
    f.engine.Update(2);
    TESSERA_CHECK(!Sees(f, 0, e));
}

TESSERA_TEST(a_large_batch_of_changes_is_evaluated_correctly) {
    Fixture f(MakeConfig()); // spatial range 100 m
    uint64_t tick = 0;
    SettleViewer(f, tick);
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> coordinate(-700.0f, 700.0f);
    std::vector<EntityId> ids;
    std::vector<std::array<float, 2>> positions;
    for (uint32_t i = 0; i < 3000; ++i) {
        const EntityId id = MakeId(kSpatialType, i);
        const std::array<float, 2> p{coordinate(rng), coordinate(rng)};
        f.grid.Insert(id, p[0], 0.0f, p[1]);
        ids.push_back(id);
        positions.push_back(p);
    }
    f.engine.Update(tick);
    size_t expected_count = 0;
    for (size_t i = 0; i < ids.size(); ++i) {
        const double dx = positions[i][0];
        const double dz = positions[i][1];
        const bool expected = dx * dx + dz * dz <= 100.0 * 100.0;
        expected_count += expected ? 1 : 0;
        TESSERA_CHECK(Sees(f, 0, ids[i]) == expected);
    }
    TESSERA_CHECK(expected_count > 0);
    TESSERA_CHECK(f.engine.VisibleCount(0) == expected_count);
}

TESSERA_TEST(replacing_an_engine_keeps_the_grid_recording_for_the_new_one) {
    SpatialGrid grid{GridConfig{64.0f, 64.0f, 64.0f}};
    RelevancyGroups groups;
    ThreadPoolJobSystem jobs(1);
    auto engine = std::make_unique<RelevancyEngine>(MakeConfig(), grid, groups, jobs);
    // The new engine is built before the old one is destroyed, as in `engine = std::make_unique<...>(...)`.
    engine = std::make_unique<RelevancyEngine>(MakeConfig(), grid, groups, jobs);
    grid.Insert(MakeId(kSpatialType, 1), 1.0f, 2.0f, 3.0f);
    TESSERA_CHECK(grid.Changes().size() == 1); // the new engine still sees what happens in the grid
    engine.reset();
    grid.Insert(MakeId(kSpatialType, 2), 4.0f, 5.0f, 6.0f);
    TESSERA_CHECK(grid.Changes().empty()); // the last engine is gone, nobody records
}
