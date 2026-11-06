// SPDX-License-Identifier: Apache-2.0
#include <cstdint>
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
