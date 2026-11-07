// SPDX-License-Identifier: Apache-2.0
// Pacing of resends, priority and the byte budget in ConnectionState::Encode.
#include "replication_rig.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <random>
#include <vector>

using namespace tessera::net;
using namespace replication_test;

namespace {

EncodeLimits Paced(uint32_t resend_after) {
    EncodeLimits limits;
    limits.resend_after_ticks = resend_after;
    return limits;
}

} // namespace

TESSERA_TEST(pacing_does_not_resend_before_resend_after_ticks) {
    Rig rig;
    const EntityId id = rig.Spawn(1.0f, 2.0f, 3.0f);
    rig.Refresh();
    rig.Enter(id, 0);
    EncodeStats stats;
    rig.EncodeWith(1, Paced(3), &stats);
    TESSERA_CHECK(stats.records == 1 && stats.waiting == 0);
    for (uint64_t tick = 2; tick <= 3; ++tick) { // sent at 1: not again before tick 4
        const std::vector<uint8_t> bytes = rig.EncodeWith(tick, Paced(3), &stats);
        TESSERA_CHECK(stats.records == 0 && stats.waiting == 1 && bytes.size() == 1);
        TESSERA_CHECK(rig.conn.IsPending(0)); // still waiting for its confirmation
    }
    rig.EncodeWith(4, Paced(3), &stats);
    TESSERA_CHECK(stats.records == 1 && stats.waiting == 0);
}

TESSERA_TEST(pacing_sends_a_changed_entity_at_once) {
    Rig rig;
    const EntityId id = rig.Spawn(1.0f, 2.0f, 3.0f);
    rig.Refresh();
    rig.Enter(id, 0);
    EncodeStats stats;
    rig.EncodeWith(1, Paced(10), &stats);
    TESSERA_CHECK(stats.records == 1);
    rig.MoveX(id, 9.0f); // new information: it does not wait for the round trip
    rig.Refresh();
    rig.EncodeWith(2, Paced(10), &stats);
    TESSERA_CHECK(stats.records == 1 && stats.waiting == 0);
}

TESSERA_TEST(pacing_resend_after_one_is_todays_behaviour) {
    Rig a;
    Rig b;
    std::mt19937 random(7);
    std::vector<EntityId> ids_a, ids_b;
    for (uint32_t i = 0; i < 12; ++i) {
        ids_a.push_back(a.Spawn(static_cast<float>(i), 1.0f, 2.0f));
        ids_b.push_back(b.Spawn(static_cast<float>(i), 1.0f, 2.0f));
    }
    a.Refresh();
    b.Refresh();
    for (uint32_t i = 0; i < 12; ++i) {
        a.Enter(ids_a[i], i);
        b.Enter(ids_b[i], i);
    }
    for (uint64_t tick = 1; tick <= 20; ++tick) {
        for (uint32_t i = 0; i < 12; ++i) {
            if (random() % 3 == 0) {
                const float x = static_cast<float>(random() % 400) / 10.0f;
                a.MoveX(ids_a[i], x);
                b.MoveX(ids_b[i], x);
            }
        }
        a.Refresh();
        b.Refresh();
        TESSERA_CHECK(a.Encode(tick) == b.EncodeWith(tick, EncodeLimits{}));
        if (tick % 4 == 0) {
            a.conn.Confirm(tick - 1);
            b.conn.Confirm(tick - 1);
        }
    }
}

TESSERA_TEST(a_confirmation_of_an_older_send_still_works_after_paced_resends) {
    Rig rig;
    const EntityId id = rig.Spawn(1.0f, 2.0f, 3.0f);
    rig.Refresh();
    rig.Enter(id, 0);
    for (const uint64_t tick : {1, 4, 7}) { // the same state, sent three times, three ticks apart
        EncodeStats stats;
        rig.EncodeWith(tick, Paced(3), &stats);
        TESSERA_CHECK(stats.records == 1);
    }
    rig.conn.Confirm(1);
    TESSERA_CHECK(rig.conn.HasBaseline(0));
    EncodeStats stats;
    rig.EncodeWith(8, Paced(3), &stats);
    TESSERA_CHECK(stats.records == 0 && !rig.conn.IsPending(0)); // it stops: the player has this state
}

TESSERA_TEST(paced_resends_of_the_same_state_each_take_their_own_ring_place) {
    for (const uint32_t sends : {8u, 9u}) {
        Rig rig(1000, 255, 8); // a ring of 8; the baseline may be old
        const EntityId id = rig.Spawn(1.0f, 2.0f, 3.0f);
        rig.Refresh();
        rig.Enter(id, 0);
        for (uint32_t i = 0; i < sends; ++i) {
            EncodeStats stats;
            rig.EncodeWith(1 + 3 * i, Paced(3), &stats);
            TESSERA_CHECK(stats.records == 1);
        }
        rig.conn.Confirm(1); // the very first send
        // Eight sends fit in the ring, so the first one is still remembered; the ninth pushed it out.
        TESSERA_CHECK(rig.conn.HasBaseline(0) == (sends == 8));
    }
}
