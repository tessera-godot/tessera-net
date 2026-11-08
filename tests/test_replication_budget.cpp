// SPDX-License-Identifier: Apache-2.0
// Pacing of resends, priority and the byte budget in ConnectionState::Encode.
#include "replication_rig.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
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

namespace {

EncodeLimits Capped(uint32_t max_bytes, uint32_t resend_after = 1000) {
    EncodeLimits limits;
    limits.max_bytes = max_bytes;
    limits.resend_after_ticks = resend_after;
    return limits;
}

// `count` mobs that the connection sees under compact ids 0 to count - 1.
struct Crowd {
    Rig rig;
    std::vector<EntityId> ids;

    explicit Crowd(uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            ids.push_back(rig.Spawn(static_cast<float>(i) * 1.5f, 1.0f, 2.0f));
        }
        rig.Refresh();
        for (uint32_t i = 0; i < count; ++i) {
            rig.Enter(ids[i], i);
        }
    }

    // Sends everything in full, applies it and confirms it.
    void SendAndConfirm(uint64_t tick) {
        TESSERA_CHECK(rig.receiver.Apply(rig.Encode(tick), tick) == DecodeStatus::Ok);
        rig.conn.Confirm(tick);
    }

    void MoveAll(float by) {
        for (uint32_t i = 0; i < ids.size(); ++i) {
            rig.MoveX(ids[i], static_cast<float>(i) * 1.5f + by);
        }
        rig.Refresh();
    }

    // How many of the first `count` entities the receiver holds exactly as the store has them.
    uint32_t Matching(uint32_t first, uint32_t count) const {
        uint32_t matching = 0;
        for (uint32_t i = first; i < first + count; ++i) {
            matching += rig.ReceiverMatchesStore(ids[i], i) ? 1 : 0;
        }
        return matching;
    }
};

} // namespace

TESSERA_TEST(a_budget_that_holds_everything_matches_the_unlimited_encoding) {
    Rig a;
    Rig b;
    std::mt19937 random(11);
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
        EncodeStats stats;
        TESSERA_CHECK(a.Encode(tick) == b.EncodeWith(tick, Capped(100000, 1), &stats));
        TESSERA_CHECK(stats.deferred == 0);
        if (tick % 4 == 0) {
            a.conn.Confirm(tick - 1);
            b.conn.Confirm(tick - 1);
        }
    }
}

TESSERA_TEST(when_it_does_not_fit_the_oldest_wait_goes_first_and_the_rest_stay_pending) {
    Crowd crowd(30);
    crowd.SendAndConfirm(1);
    crowd.MoveAll(10.0f); // all 30 have something new, all at the same tick: they tie
    uint32_t matched = 0;
    for (uint64_t tick = 2; tick <= 40 && matched < 30; ++tick) {
        EncodeStats stats;
        const std::vector<uint8_t> bytes = crowd.rig.EncodeWith(tick, Capped(40), &stats);
        TESSERA_CHECK(bytes.size() <= 40);
        TESSERA_CHECK(crowd.rig.receiver.Apply(bytes, tick) == DecodeStatus::Ok);
        TESSERA_CHECK(stats.records >= 1 && stats.deferred == 30 - matched - stats.records);
        // Ties go to the lowest id, and what was sent waits for its confirmation: a prefix grows.
        const uint32_t now = crowd.Matching(0, 30);
        TESSERA_CHECK(now == matched + stats.records && crowd.Matching(0, now) == now);
        matched = now;
        if (matched < 30) {
            TESSERA_CHECK(crowd.rig.conn.IsPending(matched)); // the first one left out is still pending
        }
    }
    TESSERA_CHECK(matched == 30);
}

TESSERA_TEST(the_longest_wait_goes_first_even_for_a_higher_id) {
    Crowd crowd(30);
    crowd.SendAndConfirm(1);
    uint32_t full = 0; // room for exactly one complete-sized record, and for nothing smaller beside it
    for (uint32_t i = 0; i < 30; ++i) {
        full = std::max(full, crowd.rig.conn.LastRecordBytes(i));
    }
    const EncodeLimits one_record = Capped(1 + full);
    crowd.MoveAll(10.0f); // everyone has something new at tick 2
    EncodeStats stats;
    TESSERA_CHECK(crowd.rig.receiver.Apply(crowd.rig.EncodeWith(2, one_record, &stats), 2) ==
                  DecodeStatus::Ok);
    TESSERA_CHECK(stats.records == 1 && crowd.Matching(0, 1) == 1); // the lowest id, as they all tie
    crowd.rig.MoveX(crowd.ids[0], 30.0f); // it changes again: new, but it has not waited
    crowd.rig.Refresh();
    TESSERA_CHECK(crowd.rig.receiver.Apply(crowd.rig.EncodeWith(3, one_record, &stats), 3) ==
                  DecodeStatus::Ok);
    // At tick 3 the entity that has waited since tick 2 (id 1) goes before id 0, which changed just now.
    TESSERA_CHECK(stats.records == 1);
    TESSERA_CHECK(crowd.Matching(1, 1) == 1 && crowd.Matching(0, 1) == 0);
}

TESSERA_TEST(a_type_weight_beats_age) {
    for (const bool weighted : {false, true}) {
        Rig rig;
        EntityTable<TestColumns> scores{TableConfig{0, kScoreType}};
        const EntityId mob = rig.Spawn(1.0f, 2.0f, 3.0f);
        const EntityId score = scores.Create();
        {
            const auto where = scores.Find(score);
            scores.Columns(where->page).score[where->slot] = 5;
            scores.MarkDirty(score);
        }
        rig.Refresh();
        rig.store.Refresh(scores, rig.jobs);
        rig.conn.MarkChanged(rig.store.Changed());
        rig.store.ClearChanged();
        rig.Enter(mob, 0);
        // The mob has waited since tick 2 and the score since tick 4; neither fits while the buffer is tiny.
        EncodeLimits tiny = Capped(3);
        std::vector<uint8_t> bytes = rig.EncodeWith(2, tiny);
        rig.Enter(score, 1);
        bytes = rig.EncodeWith(4, tiny);
        // Room for exactly one record: the largest estimate, which no two records add up to.
        const uint32_t estimate =
            std::max(rig.schema.Find(kMobType)->words, rig.schema.Find(kScoreType)->words) * 4 + 4;
        std::array<uint8_t, 256> weights{};
        weights.fill(1);
        weights[kScoreType] = weighted ? 4 : 1;
        EncodeLimits one = Capped(1 + estimate);
        one.type_weight = weights.data();
        EncodeStats stats;
        bytes = rig.EncodeWith(5, one, &stats);
        TESSERA_CHECK(stats.records == 1 && stats.deferred == 1);
        TESSERA_CHECK(rig.receiver.Apply(bytes, 5) == DecodeStatus::Ok);
        // Waited 4 ticks (the mob) against 2 (the score): 5 against 3 with equal weights, 5 against 12
        // with 4.
        TESSERA_CHECK((rig.receiver.Current(1) != nullptr) == weighted);
        TESSERA_CHECK((rig.receiver.Current(0) != nullptr) == !weighted);
    }
}

TESSERA_TEST(ties_break_by_the_lowest_compact_id) {
    Crowd crowd(10);                    // all new, all waiting since the same tick
    crowd.rig.EncodeWith(1, Capped(2)); // nothing fits, but they all start waiting
    EncodeStats stats;
    const std::vector<uint8_t> bytes = crowd.rig.EncodeWith(2, Capped(60), &stats);
    TESSERA_CHECK(stats.records >= 1 && stats.records < 10);
    TESSERA_CHECK(crowd.rig.receiver.Apply(bytes, 2) == DecodeStatus::Ok);
    TESSERA_CHECK(crowd.Matching(0, stats.records) == stats.records); // exactly the lowest ids
    TESSERA_CHECK(crowd.Matching(stats.records, 10 - stats.records) == 0);
}

TESSERA_TEST(the_buffer_never_exceeds_the_cap) {
    for (const uint32_t cap : {1u, 2u, 5u, 8u, 16u, 33u, 64u, 100u, 400u}) {
        Crowd crowd(40);
        for (uint64_t tick = 1; tick <= 60; ++tick) {
            crowd.MoveAll(static_cast<float>(tick));
            const std::vector<uint8_t> bytes = crowd.rig.EncodeWith(tick, Capped(cap, 2));
            TESSERA_CHECK(bytes.size() <= cap);
            TESSERA_CHECK(crowd.rig.receiver.Apply(bytes, tick) == DecodeStatus::Ok);
        }
    }
}

TESSERA_TEST(a_record_that_overshoots_the_estimate_is_rewound_and_deferred) {
    Crowd crowd(1);
    crowd.SendAndConfirm(1); // a complete record: the entity's last size is a full record
    crowd.MoveAll(0.5f);
    TESSERA_CHECK(crowd.rig.receiver.Apply(crowd.rig.Encode(2), 2) ==
                  DecodeStatus::Ok); // a small partial record
    crowd.rig.conn.Confirm(2);
    // Long after, the confirmed state is too old to write against, so the next record is complete and larger
    // than the small one the estimate is based on.
    crowd.MoveAll(3.0f);
    EncodeStats stats;
    const std::vector<uint8_t> bytes = crowd.rig.EncodeWith(60, Capped(8), &stats);
    TESSERA_CHECK(bytes.size() == 1 && stats.records == 0 && stats.deferred == 1);
    TESSERA_CHECK(crowd.rig.conn.IsPending(0));
    stats = EncodeStats{};
    const std::vector<uint8_t> later = crowd.rig.EncodeWith(61, Capped(200), &stats); // room for it
    TESSERA_CHECK(stats.records == 1 && stats.complete_records == 1 && later.size() > 8);
    TESSERA_CHECK(crowd.rig.receiver.Apply(later, 61) == DecodeStatus::Ok);
    TESSERA_CHECK(crowd.rig.ReceiverMatchesStore(crowd.ids[0], 0));
}

TESSERA_TEST(no_entity_starves_under_a_constant_overload) {
    Crowd crowd(40);
    uint32_t least_records = 1000;
    uint32_t worst_lag = 0;
    for (uint64_t tick = 1; tick <= 120; ++tick) {
        crowd.MoveAll(static_cast<float>(tick) * 0.3f); // everything changes on every tick
        EncodeStats stats;
        const std::vector<uint8_t> bytes = crowd.rig.EncodeWith(tick, Capped(140, 1), &stats);
        TESSERA_CHECK(crowd.rig.receiver.Apply(bytes, tick) == DecodeStatus::Ok);
        if (tick > 40) {
            least_records = std::min(least_records, stats.records);
            for (uint32_t i = 0; i < 40; ++i) {
                worst_lag = std::max<uint32_t>(
                    worst_lag, static_cast<uint32_t>(tick - crowd.rig.receiver.CurrentTick(i)));
            }
        }
    }
    TESSERA_CHECK(least_records >= 3);
    // Round robin: everyone is sent within the number of ticks it takes to go through all of them, plus a
    // little.
    TESSERA_CHECK(worst_lag <= (40 + least_records - 1) / least_records + 2);
}

TESSERA_TEST(the_per_type_estimate_adapts_to_the_records_written) {
    Crowd crowd(30);
    EncodeStats first, second;
    crowd.rig.receiver.Apply(crowd.rig.EncodeWith(1, Capped(200, 1000), &first), 1);
    crowd.rig.receiver.Apply(crowd.rig.EncodeWith(2, Capped(200, 1000), &second), 2);
    // The first guess is room for every word; once real records have been written the guess comes down.
    TESSERA_CHECK(first.records >= 1 && second.records > first.records);
}

TESSERA_TEST(an_entity_whose_estimate_is_too_small_is_not_left_out_for_ever) {
    Crowd crowd(4);
    crowd.SendAndConfirm(1); // four complete records
    uint32_t full = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        full = std::max(full, crowd.rig.conn.LastRecordBytes(i));
    }
    // The last entity takes a tiny step and is sent as a small partial record, so its estimate is small.
    crowd.rig.MoveX(crowd.ids[3], 3 * 1.5f + 0.01f);
    crowd.rig.Refresh();
    TESSERA_CHECK(crowd.rig.receiver.Apply(crowd.rig.EncodeWith(2, Capped(200)), 2) == DecodeStatus::Ok);
    crowd.rig.conn.Confirm(2);
    TESSERA_CHECK(crowd.rig.conn.LastRecordBytes(3) < full);
    // Long after, the confirmed state is too old to write against: every record is complete. Room for two of
    // them and the small estimate of the last, which is its record's real size short by the difference.
    const uint32_t cap = 1 + 2 * full + crowd.rig.conn.LastRecordBytes(3);
    for (uint64_t tick = 60; tick < 80; ++tick) {
        crowd.MoveAll(static_cast<float>(tick) * 0.3f);
        const std::vector<uint8_t> bytes = crowd.rig.EncodeWith(tick, Capped(cap));
        TESSERA_CHECK(bytes.size() <= cap);
        TESSERA_CHECK(crowd.rig.receiver.Apply(bytes, tick) == DecodeStatus::Ok);
    }
    TESSERA_CHECK(crowd.rig.receiver.CurrentTick(3) >= 60);
}

TESSERA_TEST(a_late_confirmation_does_not_leave_an_old_wait_behind) {
    Crowd crowd(2);
    TESSERA_CHECK(crowd.rig.receiver.Apply(crowd.rig.EncodeWith(1, Capped(200, 3)), 1) == DecodeStatus::Ok);
    uint32_t full = 0;
    for (uint32_t i = 0; i < 2; ++i) {
        full = std::max(full, crowd.rig.conn.LastRecordBytes(i));
    }
    // Sent at tick 1 and due again at tick 4, but nothing fits: both start to wait there. The confirmation
    // comes after that, and they are idle.
    crowd.rig.EncodeWith(4, Capped(2, 3));
    crowd.rig.conn.Confirm(1);
    // Much later the second entity changes, and nothing fits: it waits from tick 950.
    crowd.rig.MoveX(crowd.ids[1], 70.0f);
    crowd.rig.Refresh();
    crowd.rig.EncodeWith(950, Capped(2, 3));
    // At tick 1000 both change and there is room for one complete record. The second has waited for 50 ticks,
    // the first for none: the old wait from tick 4 must not count.
    crowd.MoveAll(40.0f);
    EncodeStats stats;
    TESSERA_CHECK(crowd.rig.receiver.Apply(crowd.rig.EncodeWith(1000, Capped(1 + full, 3), &stats), 1000) ==
                  DecodeStatus::Ok);
    TESSERA_CHECK(stats.records == 1);
    TESSERA_CHECK(crowd.Matching(1, 1) == 1 && crowd.Matching(0, 1) == 0);
}
