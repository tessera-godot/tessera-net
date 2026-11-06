// SPDX-License-Identifier: Apache-2.0
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>

#include "relevancy_test_support.hpp"
#include "test_support.hpp"

using namespace relevancy_test;
using Kind = tessera::net::VisibilityEvent::Kind;

TESSERA_TEST(engine_rejects_invalid_config) {
    RelevancyConfig config = MakeConfig();
    config.recalc_interval_ticks = 0;
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

TESSERA_TEST(always_relevant_entity_enters_only_on_the_connections_due_tick) {
    Fixture f(MakeConfig(3));
    f.engine.AddConnection(1);
    const EntityId e = MakeId(kGlobalType, 1);
    f.engine.OnEntityCreated(e);

    uint32_t processed = 99;
    f.engine.Update(0, &processed);  // connection 1 is due when tick % 3 == 1
    TESSERA_CHECK(processed == 0);
    TESSERA_CHECK(f.engine.Events(1).events.empty());

    f.engine.Update(1, &processed);
    TESSERA_CHECK(processed == 1);
    TESSERA_CHECK(f.engine.Events(1).events.size() == 1);
    TESSERA_CHECK(f.engine.Events(1).events[0].kind == Kind::Enter);
    TESSERA_CHECK(f.engine.Events(1).events[0].id == e);
}

TESSERA_TEST(always_relevant_entities_get_compact_ids_in_raw_order) {
    Fixture f(MakeConfig(1));
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
    Fixture f(MakeConfig(1));
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
    Fixture f(MakeConfig(1));
    f.engine.AddConnection(0);
    const EntityId owned = MakeId(kOwnedType, 1);
    const EntityId spatial = MakeId(kSpatialType, 2);
    f.engine.OnEntityCreated(owned);
    f.engine.OnEntityCreated(spatial);
    TESSERA_CHECK(!f.engine.SetOwner(MakeId(kOwnedType, 77), 0));  // never created
    TESSERA_CHECK(!f.engine.SetOwner(spatial, 0));                 // type is not owner-only
    TESSERA_CHECK(!f.engine.SetOwner(owned, 5));                   // connection not active
    TESSERA_CHECK(f.engine.SetOwner(owned, 0));
}

TESSERA_TEST(reassigning_the_owner_moves_visibility) {
    Fixture f(MakeConfig(1));
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
    Fixture f(MakeConfig(1));
    f.engine.AddConnection(0);
    f.engine.OnEntityCreated(MakeId(kGlobalType, 1));
    f.engine.Update(0);
    f.engine.OnEntityCreated(MakeId(kGlobalType, 2));
    f.engine.Update(1);
    TESSERA_CHECK(f.engine.Events(0).events.size() == 2);  // not cleared in between
    f.engine.ClearEvents(0);
    TESSERA_CHECK(f.engine.Events(0).events.empty());
}

TESSERA_TEST(compact_id_of_is_invalid_for_entities_that_are_not_visible) {
    Fixture f(MakeConfig(1));
    f.engine.AddConnection(0);
    TESSERA_CHECK(f.engine.CompactIdOf(0, MakeId(kGlobalType, 1)) == kInvalidCompactId);
}

TESSERA_TEST(queries_on_unknown_connections_are_safe) {
    Fixture f(MakeConfig(1));
    TESSERA_CHECK(f.engine.Events(3).events.empty());
    TESSERA_CHECK(f.engine.VisibleCount(3) == 0);
    TESSERA_CHECK(f.engine.VisibleOverflow(3) == 0);
    TESSERA_CHECK(f.engine.CompactIdOf(3, MakeId(kGlobalType, 1)) == kInvalidCompactId);
    TESSERA_CHECK(f.engine.CompactIdOf(9999, MakeId(kGlobalType, 1)) == kInvalidCompactId);
    f.engine.ClearEvents(3);  // no effect, no crash
}

// ---- Spatial mode, hysteresis and observers --------------------------------------------------------

namespace {

void Move(Fixture& f, EntityId id, float x, float y = 0.0f, float z = 0.0f) {
    f.grid.ApplyMoves({PendingMove{id, x, y, z}});
}

}  // namespace

TESSERA_TEST(spatial_entity_enters_within_range_including_the_border) {
    Fixture f(MakeConfig(1));
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
    RelevancyConfig config = MakeConfig(1);
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
    TESSERA_CHECK(f.engine.CompactIdOf(0, e) != kInvalidCompactId);  // exactly range + margin

    Move(f, e, 110.01f);
    f.engine.Update(2);
    TESSERA_CHECK(f.engine.CompactIdOf(0, e) == kInvalidCompactId);
}

TESSERA_TEST(hidden_entity_inside_the_margin_does_not_enter) {
    RelevancyConfig config = MakeConfig(1);
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
    RelevancyConfig config = MakeConfig(1);
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
    Fixture f(MakeConfig(1));
    f.engine.AddConnection(0);
    const Observer eye{0.0f, 0.0f, 0.0f};
    f.engine.SetObservers(0, std::span<const Observer>(&eye, 1));
    const EntityId near_type = MakeId(kSpatialType, 1);  // range 100
    const EntityId far_type = MakeId(kFarType, 2);       // range 500
    f.grid.Insert(near_type, 200.0f, 0.0f, 0.0f);
    f.grid.Insert(far_type, 200.0f, 0.0f, 0.0f);
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.CompactIdOf(0, near_type) == kInvalidCompactId);
    TESSERA_CHECK(f.engine.CompactIdOf(0, far_type) != kInvalidCompactId);
}

TESSERA_TEST(zero_range_type_is_never_visible_even_at_the_observer) {
    Fixture f(MakeConfig(1));
    f.engine.AddConnection(0);
    const Observer eye{0.0f, 0.0f, 0.0f};
    f.engine.SetObservers(0, std::span<const Observer>(&eye, 1));
    const EntityId e = MakeId(kNoRangeType, 1);
    f.grid.Insert(e, 0.0f, 0.0f, 0.0f);
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.CompactIdOf(0, e) == kInvalidCompactId);
}

TESSERA_TEST(connection_without_observers_sees_global_and_owned_but_not_spatial) {
    Fixture f(MakeConfig(1));
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
    Fixture f(MakeConfig(1));
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
    Fixture f(MakeConfig(1));
    f.engine.AddConnection(0);
    const Observer eyes[] = {{0.0f, 0.0f, 0.0f}, {1000.0f, 0.0f, 0.0f}};
    f.engine.SetObservers(0, eyes);
    const EntityId a = MakeId(kSpatialType, 1);
    const EntityId b = MakeId(kSpatialType, 2);
    const EntityId c = MakeId(kSpatialType, 3);
    f.grid.Insert(a, 10.0f, 0.0f, 0.0f);
    f.grid.Insert(b, 1010.0f, 0.0f, 0.0f);
    f.grid.Insert(c, 500.0f, 0.0f, 0.0f);  // far from both
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.CompactIdOf(0, a) != kInvalidCompactId);
    TESSERA_CHECK(f.engine.CompactIdOf(0, b) != kInvalidCompactId);
    TESSERA_CHECK(f.engine.CompactIdOf(0, c) == kInvalidCompactId);
    TESSERA_CHECK(f.engine.VisibleCount(0) == 2);
}

TESSERA_TEST(entity_seen_by_two_observers_enters_once) {
    Fixture f(MakeConfig(1));
    f.engine.AddConnection(0);
    const Observer eyes[] = {{0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}};
    f.engine.SetObservers(0, eyes);
    f.grid.Insert(MakeId(kSpatialType, 1), 5.0f, 0.0f, 0.0f);
    f.engine.Update(0);
    TESSERA_CHECK(CountEvents(f.engine.Events(0), Kind::Enter) == 1);
}

TESSERA_TEST(set_observers_rejects_bad_input_and_keeps_the_previous_list) {
    RelevancyConfig config = MakeConfig(1);
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
    TESSERA_CHECK(f.engine.CompactIdOf(0, e) != kInvalidCompactId);  // the first list still applies
}

TESSERA_TEST(spatial_pass_never_shows_owner_only_entities_to_other_connections) {
    Fixture f(MakeConfig(1));
    f.engine.AddConnection(0);
    f.engine.AddConnection(1);
    const Observer eye{0.0f, 0.0f, 0.0f};
    f.engine.SetObservers(0, std::span<const Observer>(&eye, 1));
    const EntityId e = MakeId(kOwnedType, 1);
    f.engine.OnEntityCreated(e);
    f.engine.SetOwner(e, 1);
    f.grid.Insert(e, 0.0f, 0.0f, 0.0f);  // standing right next to connection 0 observer
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.CompactIdOf(0, e) == kInvalidCompactId);
    TESSERA_CHECK(f.engine.CompactIdOf(1, e) != kInvalidCompactId);
}

TESSERA_TEST(global_entity_in_the_grid_enters_only_once) {
    Fixture f(MakeConfig(1));
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
    Fixture f(MakeConfig(1));
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

}  // namespace

TESSERA_TEST(exclusion_hides_a_spatial_entity_and_makes_a_visible_one_leave) {
    Fixture f(MakeConfig(1));
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

    f.groups.AddMember(g, seen);  // now an already visible entity is vetoed
    f.engine.Update(1);
    TESSERA_CHECK(f.engine.CompactIdOf(0, seen) == kInvalidCompactId);
    TESSERA_CHECK(CountEvents(f.engine.Events(0), Kind::Leave) == 1);
}

TESSERA_TEST(exclusion_hides_global_and_owned_entities) {
    Fixture f(MakeConfig(1));
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
    Fixture f(MakeConfig(1));
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
    Fixture f(MakeConfig(1));
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
    Fixture f(MakeConfig(1));
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
    Fixture f(MakeConfig(1));
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
    Fixture f(MakeConfig(1));
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

TESSERA_TEST(destroy_is_immediate_even_when_the_connection_is_not_due) {
    Fixture f(MakeConfig(3));
    f.engine.AddConnection(0);  // due on ticks where tick % 3 == 0
    const EntityId e = MakeId(kGlobalType, 1);
    f.engine.OnEntityCreated(e);
    f.engine.Update(0);
    const uint32_t compact = f.engine.CompactIdOf(0, e);
    TESSERA_CHECK(compact != kInvalidCompactId);
    f.engine.ClearEvents(0);

    f.engine.OnEntityDestroyed(e);
    uint32_t processed = 99;
    f.engine.Update(1, &processed);  // connection 0 is not due now
    TESSERA_CHECK(processed == 0);
    TESSERA_CHECK(f.engine.Events(0).events.size() == 1);
    TESSERA_CHECK(f.engine.Events(0).events[0].kind == Kind::Destroy);
    TESSERA_CHECK(f.engine.Events(0).events[0].id == e);
    TESSERA_CHECK(f.engine.Events(0).events[0].compact_id == compact);
    TESSERA_CHECK(f.engine.VisibleCount(0) == 0);
}

TESSERA_TEST(destroying_an_unknown_id_does_nothing) {
    Fixture f(MakeConfig(1));
    f.engine.AddConnection(0);
    f.engine.OnEntityDestroyed(MakeId(kSpatialType, 99));
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.Events(0).events.empty());
}

TESSERA_TEST(destroying_the_same_id_twice_emits_one_destroy) {
    Fixture f(MakeConfig(1));
    f.engine.AddConnection(0);
    const EntityId e = MakeId(kGlobalType, 1);
    f.engine.OnEntityCreated(e);
    f.engine.Update(0);
    f.engine.ClearEvents(0);
    f.engine.OnEntityDestroyed(e);
    f.engine.OnEntityDestroyed(e);
    f.engine.Update(1);
    f.engine.OnEntityDestroyed(e);  // again, in a later tick
    f.engine.Update(2);
    TESSERA_CHECK(CountEvents(f.engine.Events(0), Kind::Destroy) == 1);
}

TESSERA_TEST(destroyed_entity_leaves_groups_and_is_not_reentered_by_stale_inclusion) {
    Fixture f(MakeConfig(1));
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
    Fixture f(MakeConfig(1));
    const EntityId e = MakeId(kGlobalType, 1);
    f.engine.OnEntityCreated(e);
    f.engine.OnEntityDestroyed(e);
    f.engine.Update(0);
    f.engine.AddConnection(0);
    f.engine.Update(1);
    TESSERA_CHECK(f.engine.VisibleCount(0) == 0);
}

TESSERA_TEST(reused_connection_id_starts_clean) {
    Fixture f(MakeConfig(1));
    f.engine.AddConnection(0);
    const EntityId e = MakeId(kGlobalType, 1);
    f.engine.OnEntityCreated(e);
    const GroupId g = f.groups.CreateGroup(GroupKind::Exclusion);
    f.groups.SetStatus(g, 0, true);
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.VisibleCount(0) == 1);

    TESSERA_CHECK(f.engine.RemoveConnection(0));
    TESSERA_CHECK(!f.groups.IsAllowed(g, 0));  // the old exception is gone
    TESSERA_CHECK(f.engine.AddConnection(0));
    TESSERA_CHECK(f.engine.VisibleCount(0) == 0);
    TESSERA_CHECK(f.engine.Events(0).events.empty());
    f.engine.Update(1);
    TESSERA_CHECK(f.engine.Events(0).events.size() == 1);
    TESSERA_CHECK(f.engine.Events(0).events[0].compact_id == 0);  // compact ids start over
}

TESSERA_TEST(entities_of_a_removed_owner_become_ownerless_and_can_be_given_again) {
    Fixture f(MakeConfig(1));
    f.engine.AddConnection(0);
    const EntityId e = MakeId(kOwnedType, 1);
    f.engine.OnEntityCreated(e);
    f.engine.SetOwner(e, 0);
    f.engine.RemoveConnection(0);
    f.engine.AddConnection(0);
    f.engine.Update(0);
    TESSERA_CHECK(f.engine.VisibleCount(0) == 0);  // the new connection 0 does not inherit it
    TESSERA_CHECK(f.engine.SetOwner(e, 0));
    f.engine.Update(1);
    TESSERA_CHECK(f.engine.CompactIdOf(0, e) != kInvalidCompactId);
}

TESSERA_TEST(overflow_keeps_extra_entities_out_and_counts_them) {
    RelevancyConfig config = MakeConfig(1);
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
    RelevancyConfig config = MakeConfig(1);
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
    Fixture f(MakeConfig(1));
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
