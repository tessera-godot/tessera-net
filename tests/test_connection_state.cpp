// SPDX-License-Identifier: Apache-2.0
#include "replication_test_support.hpp"
#include "tessera/net/replication/connection_state.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <vector>

using namespace tessera::net;
using namespace replication_test;

namespace {

VisibilityEvent Enter(EntityId id, uint32_t compact_id) {
    return VisibilityEvent{VisibilityEvent::Kind::Enter, id, compact_id};
}
VisibilityEvent Leave(EntityId id, uint32_t compact_id) {
    return VisibilityEvent{VisibilityEvent::Kind::Leave, id, compact_id};
}
VisibilityEvent Destroy(EntityId id, uint32_t compact_id) {
    return VisibilityEvent{VisibilityEvent::Kind::Destroy, id, compact_id};
}

constexpr uint32_t kMaxAge = 32;

} // namespace

TESSERA_TEST(enter_tracks_the_entity_as_pending_without_a_baseline) {
    const ReplicationSchema schema = MakeTestSchema();
    ConnectionState connection(schema, 1000, kMaxAge);
    const EntityId a = MakeEntity(kMobType, 10);
    const EntityId b = MakeEntity(kMobType, 20);
    const VisibilityEvent events[] = {Enter(a, 0), Enter(b, 5)};
    connection.ApplyEvents(events);
    TESSERA_CHECK(connection.VisibleCount() == 2);
    TESSERA_CHECK(connection.CompactIdOf(a) == 0 && connection.CompactIdOf(b) == 5);
    TESSERA_CHECK(connection.EntityOf(0) == a && connection.EntityOf(5) == b);
    TESSERA_CHECK(connection.IsPending(0) && connection.IsPending(5));
    TESSERA_CHECK(!connection.HasBaseline(0) && !connection.HasBaseline(5));
    TESSERA_CHECK(!connection.IsPending(1)); // an id nobody uses
}

TESSERA_TEST(leave_and_destroy_release_the_record_and_clear_the_bits) {
    const ReplicationSchema schema = MakeTestSchema();
    ConnectionState connection(schema, 1000, kMaxAge);
    const EntityId a = MakeEntity(kMobType, 10);
    const EntityId b = MakeEntity(kMobType, 20);
    const VisibilityEvent enter[] = {Enter(a, 0), Enter(b, 5)};
    connection.ApplyEvents(enter);
    const VisibilityEvent gone[] = {Destroy(b, 5), Leave(a, 0)};
    connection.ApplyEvents(gone);
    TESSERA_CHECK(connection.VisibleCount() == 0);
    TESSERA_CHECK(!connection.IsPending(0) && !connection.IsPending(5));
    TESSERA_CHECK(connection.CompactIdOf(a) == kInvalidCompactId &&
                  connection.CompactIdOf(b) == kInvalidCompactId);
    TESSERA_CHECK(connection.EntityOf(0).IsNull() && connection.EntityOf(5).IsNull());
}

TESSERA_TEST(events_in_the_relevancy_order_are_applied) {
    const ReplicationSchema schema = MakeTestSchema();
    ConnectionState connection(schema, 1000, kMaxAge);
    const EntityId e10 = MakeEntity(kMobType, 10), e20 = MakeEntity(kMobType, 20),
                   e30 = MakeEntity(kMobType, 30);
    const VisibilityEvent first[] = {Enter(e10, 0), Enter(e20, 1), Enter(e30, 2)};
    connection.ApplyEvents(first);
    // The relevancy engine reports Destroy, then Leave, then Enter, each ascending by id.
    const EntityId e15 = MakeEntity(kMobType, 15), e25 = MakeEntity(kMobType, 25);
    const VisibilityEvent second[] = {Destroy(e30, 2), Leave(e10, 0), Enter(e15, 3), Enter(e25, 4)};
    connection.ApplyEvents(second);
    TESSERA_CHECK(connection.VisibleCount() == 3);
    TESSERA_CHECK(connection.CompactIdOf(e20) == 1 && connection.CompactIdOf(e15) == 3 &&
                  connection.CompactIdOf(e25) == 4);
    TESSERA_CHECK(connection.CompactIdOf(e10) == kInvalidCompactId &&
                  connection.CompactIdOf(e30) == kInvalidCompactId);
}

TESSERA_TEST(destroy_and_enter_of_the_same_compact_id_in_one_call) {
    const ReplicationSchema schema = MakeTestSchema();
    ConnectionState connection(schema, 1000, kMaxAge);
    const EntityId a = MakeEntity(kMobType, 10), b = MakeEntity(kMobType, 20);
    const VisibilityEvent first[] = {Enter(a, 1)};
    connection.ApplyEvents(first);
    const VisibilityEvent second[] = {Destroy(a, 1), Enter(b, 1)};
    connection.ApplyEvents(second);
    TESSERA_CHECK(connection.CompactIdOf(b) == 1 && connection.CompactIdOf(a) == kInvalidCompactId);
    TESSERA_CHECK(connection.EntityOf(1) == b && connection.IsPending(1) && !connection.HasBaseline(1));
}

TESSERA_TEST(leave_for_an_unknown_entity_changes_nothing) {
    const ReplicationSchema schema = MakeTestSchema();
    ConnectionState connection(schema, 1000, kMaxAge);
    const EntityId a = MakeEntity(kMobType, 10);
    const VisibilityEvent first[] = {Enter(a, 0)};
    connection.ApplyEvents(first);
    const VisibilityEvent stray[] = {Leave(MakeEntity(kMobType, 99), 7),
                                     Destroy(MakeEntity(kMobType, 98), 8)};
    connection.ApplyEvents(stray);
    TESSERA_CHECK(connection.VisibleCount() == 1 && connection.CompactIdOf(a) == 0);
}

TESSERA_TEST(entity_of_a_type_without_schema_is_not_tracked) {
    const ReplicationSchema schema = MakeTestSchema();
    ConnectionState connection(schema, 1000, kMaxAge);
    const EntityId unknown = MakeEntity(kUnknownType, 4);
    const VisibilityEvent events[] = {Enter(unknown, 0)};
    connection.ApplyEvents(events);
    TESSERA_CHECK(connection.VisibleCount() == 0);
    TESSERA_CHECK(connection.CompactIdOf(unknown) == kInvalidCompactId);
    TESSERA_CHECK(connection.UntrackedEnters() == 1);
    const VisibilityEvent gone[] = {Leave(unknown, 0)}; // the matching Leave is harmless
    connection.ApplyEvents(gone);
    TESSERA_CHECK(connection.VisibleCount() == 0);
}

TESSERA_TEST(compact_id_beyond_the_maximum_is_ignored) {
    const ReplicationSchema schema = MakeTestSchema();
    ConnectionState connection(schema, 100, kMaxAge);
    const EntityId a = MakeEntity(kMobType, 1), b = MakeEntity(kMobType, 2);
    const VisibilityEvent events[] = {Enter(a, 100), Enter(b, 99)};
    connection.ApplyEvents(events);
    TESSERA_CHECK(connection.VisibleCount() == 1);
    TESSERA_CHECK(connection.CompactIdOf(a) == kInvalidCompactId && connection.CompactIdOf(b) == 99);
    TESSERA_CHECK(connection.UntrackedEnters() == 1);
    TESSERA_CHECK(!connection.IsPending(100) &&
                  !connection.HasBaseline(100)); // asking beyond the maximum is safe
    TESSERA_CHECK(connection.EntityOf(100).IsNull());
}

TESSERA_TEST(enter_for_an_id_already_in_use_replaces_the_old_entity) {
    const ReplicationSchema schema = MakeTestSchema();
    ConnectionState connection(schema, 1000, kMaxAge);
    const EntityId a = MakeEntity(kMobType, 10), b = MakeEntity(kMobType, 20);
    const VisibilityEvent first[] = {Enter(a, 3)};
    connection.ApplyEvents(first);
    const VisibilityEvent second[] = {Enter(b, 3)}; // the same number for another entity
    connection.ApplyEvents(second);
    TESSERA_CHECK(connection.VisibleCount() == 1);
    TESSERA_CHECK(connection.CompactIdOf(a) == kInvalidCompactId && connection.CompactIdOf(b) == 3);
    TESSERA_CHECK(connection.EntityOf(3) == b && connection.IsPending(3) && !connection.HasBaseline(3));
    const VisibilityEvent third[] = {Enter(b, 4)}; // the same entity under a new number
    connection.ApplyEvents(third);
    TESSERA_CHECK(connection.VisibleCount() == 1 && connection.CompactIdOf(b) == 4);
    TESSERA_CHECK(connection.EntityOf(3).IsNull() && !connection.IsPending(3));
}

TESSERA_TEST(mark_changed_ignores_entities_the_connection_does_not_see) {
    const ReplicationSchema schema = MakeTestSchema();
    ConnectionState connection(schema, 1000, kMaxAge);
    const EntityId seen = MakeEntity(kMobType, 10);
    const VisibilityEvent events[] = {Enter(seen, 0)};
    connection.ApplyEvents(events);
    const EntityId changed[] = {MakeEntity(kMobType, 5), seen, MakeEntity(kMobType, 50), EntityId::Null()};
    connection.MarkChanged(changed);
    TESSERA_CHECK(connection.VisibleCount() == 1);
    TESSERA_CHECK(connection.IsPending(0));
    TESSERA_CHECK(connection.CompactIdOf(MakeEntity(kMobType, 5)) == kInvalidCompactId);
}

TESSERA_TEST(memory_scales_with_the_ids_in_use_not_the_maximum) {
    const ReplicationSchema schema = MakeTestSchema();
    ConnectionState connection(schema, 65536, kMaxAge);
    std::vector<VisibilityEvent> events;
    for (uint32_t i = 0; i < 100; ++i) {
        events.push_back(Enter(MakeEntity(kMobType, i), i));
    }
    connection.ApplyEvents(events);
    TESSERA_CHECK(connection.MemoryBytes() < 128 * 1024); // one page of 256 ids, not the 65,536 allowed
    const size_t before = connection.MemoryBytes();
    const VisibilityEvent far[] = {Enter(MakeEntity(kMobType, 100'000), 60'000)};
    connection.ApplyEvents(far);
    TESSERA_CHECK(connection.MemoryBytes() > before); // a second page of records
    TESSERA_CHECK(connection.MemoryBytes() < 256 * 1024);
}

TESSERA_TEST(compact_id_of_and_entity_of_are_inverse) {
    const ReplicationSchema schema = MakeTestSchema();
    ConnectionState connection(schema, 1000, kMaxAge);
    std::vector<VisibilityEvent> events;
    for (uint32_t i = 0; i < 300; ++i) { // crosses a page boundary of the records
        events.push_back(Enter(MakeEntity(kMobType, 1000 + 3 * i), i));
    }
    connection.ApplyEvents(events);
    TESSERA_CHECK(connection.VisibleCount() == 300);
    for (uint32_t i = 0; i < 300; ++i) {
        const EntityId id = MakeEntity(kMobType, 1000 + 3 * i);
        TESSERA_CHECK(connection.CompactIdOf(id) == i);
        TESSERA_CHECK(connection.EntityOf(i) == id);
    }
}

TESSERA_TEST(apply_events_reports_what_it_started_and_stopped_tracking_in_order) {
    const ReplicationSchema schema = MakeTestSchema();
    ConnectionState connection(schema, 1000, kMaxAge);
    const EntityId a = MakeEntity(kMobType, 10), b = MakeEntity(kMobType, 20), c = MakeEntity(kMobType, 30);
    const VisibilityEvent first[] = {Enter(a, 0), Enter(b, 1)};
    connection.ApplyEvents(first);
    auto changes = connection.LastChanges();
    TESSERA_CHECK(changes.size() == 2);
    TESSERA_CHECK(changes[0].added && changes[0].raw == a.raw() && changes[0].compact_id == 0);
    TESSERA_CHECK(changes[1].added && changes[1].raw == b.raw() && changes[1].compact_id == 1);

    const VisibilityEvent second[] = {Destroy(a, 0), Leave(b, 1), Enter(c, 0)};
    connection.ApplyEvents(second);
    changes = connection.LastChanges(); // only this call: the departures first, then the arrival
    TESSERA_CHECK(changes.size() == 3);
    TESSERA_CHECK(!changes[0].added && changes[0].raw == a.raw());
    TESSERA_CHECK(!changes[1].added && changes[1].raw == b.raw());
    TESSERA_CHECK(changes[2].added && changes[2].raw == c.raw() && changes[2].compact_id == 0);

    const VisibilityEvent ignored[] = {Leave(MakeEntity(kMobType, 99), 5),
                                       Enter(MakeEntity(kUnknownType, 1), 6)};
    connection.ApplyEvents(ignored); // nothing was tracked or forgotten
    TESSERA_CHECK(connection.LastChanges().empty());
}

TESSERA_TEST(a_replaced_entity_is_reported_as_stopped_before_the_new_one_started) {
    const ReplicationSchema schema = MakeTestSchema();
    ConnectionState connection(schema, 1000, kMaxAge);
    const EntityId a = MakeEntity(kMobType, 10), b = MakeEntity(kMobType, 20);
    const VisibilityEvent events[] = {Enter(a, 3), Enter(b, 3)}; // the same number twice in one call
    connection.ApplyEvents(events);
    const auto changes = connection.LastChanges();
    TESSERA_CHECK(changes.size() == 3);
    TESSERA_CHECK(changes[0].added && changes[0].raw == a.raw());
    TESSERA_CHECK(!changes[1].added && changes[1].raw == a.raw());
    TESSERA_CHECK(changes[2].added && changes[2].raw == b.raw());
    TESSERA_CHECK(connection.CompactIdOf(b) == 3 && connection.CompactIdOf(a) == kInvalidCompactId);
}

TESSERA_TEST(mark_pending_concurrently_flags_like_mark_pending_at_and_ignores_the_same_ids) {
    const ReplicationSchema schema = MakeTestSchema();
    ConnectionState connection(schema, 1000, kMaxAge);
    const EntityId a = MakeEntity(kMobType, 10);
    const VisibilityEvent events[] = {Enter(a, 4)};
    connection.ApplyEvents(events);
    connection.MarkPendingConcurrently(4);
    TESSERA_CHECK(connection.IsPending(4));
    connection.MarkPendingConcurrently(5);    // free: nothing happens
    connection.MarkPendingConcurrently(5000); // beyond the maximum: nothing happens
    TESSERA_CHECK(!connection.IsPending(5) && !connection.IsPending(5000));
    TESSERA_CHECK(connection.Visible().size() == 1 && connection.Visible()[0].raw == a.raw());
}

TESSERA_TEST(mark_pending_at_flags_a_compact_id_without_looking_the_entity_up) {
    const ReplicationSchema schema = MakeTestSchema();
    ConnectionState connection(schema, 1000, kMaxAge);
    const EntityId a = MakeEntity(kMobType, 10);
    const VisibilityEvent events[] = {Enter(a, 4)};
    connection.ApplyEvents(events);
    connection.MarkPendingAt(4);
    TESSERA_CHECK(connection.IsPending(4));
    connection.MarkPendingAt(5);    // free: nothing happens
    connection.MarkPendingAt(5000); // beyond the maximum: nothing happens
    TESSERA_CHECK(!connection.IsPending(5) && !connection.IsPending(5000));
    TESSERA_CHECK(connection.Visible().size() == 1 && connection.Visible()[0].raw == a.raw());
}
