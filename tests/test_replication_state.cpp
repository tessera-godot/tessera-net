// SPDX-License-Identifier: Apache-2.0
#include "replication_rig.hpp"
#include "tessera/net/replication/record_codec.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <random>
#include <vector>

using namespace tessera::net;
using namespace replication_test;

TESSERA_TEST(first_record_of_an_entity_is_complete_and_the_receiver_matches_the_store) {
    Rig rig;
    const EntityId id = rig.Spawn(1.5f, 2.5f, -3.5f);
    rig.Refresh();
    rig.Enter(id, 0);
    EncodeStats stats;
    const std::vector<uint8_t> bytes = rig.Encode(1, &stats);
    TESSERA_CHECK(stats.records == 1 && stats.complete_records == 1);
    size_t applied = 0;
    TESSERA_CHECK(rig.receiver.Apply(bytes, 1, &applied) == DecodeStatus::Ok && applied == 1);
    TESSERA_CHECK(rig.ReceiverMatchesStore(id, 0));
    TESSERA_CHECK(rig.receiver.CurrentTick(0) == 1);
}

TESSERA_TEST(partial_record_marks_only_the_changed_properties) {
    Rig rig;
    const EntityId id = rig.Spawn(1.0f, 2.0f, 3.0f);
    rig.Refresh();
    rig.Enter(id, 0);
    TESSERA_CHECK(rig.receiver.Apply(rig.Encode(1), 1) == DecodeStatus::Ok);
    rig.conn.Confirm(1);
    rig.MoveX(id, 6.0f);
    rig.Refresh();
    EncodeStats stats;
    const std::vector<uint8_t> bytes = rig.Encode(2, &stats);
    TESSERA_CHECK(stats.records == 1 && stats.complete_records == 0);
    BitReader reader(bytes);
    uint32_t gap = 0, age = 0;
    bool complete = true;
    TESSERA_CHECK(ReadRecordHead(reader, &gap, &complete, &age));
    TESSERA_CHECK(gap == 1 && !complete && age == 1);
    TESSERA_CHECK(reader.ReadBits(5) == 0b00001); // only the position changed
    TESSERA_CHECK(rig.receiver.Apply(bytes, 2) == DecodeStatus::Ok);
    TESSERA_CHECK(rig.ReceiverMatchesStore(id, 0));
}

TESSERA_TEST(record_bytes_match_a_fixed_example) {
    Rig rig;
    const EntityId id = rig.Spawn(1.5f, 2.5f, -3.5f);
    auto ref = rig.At(id);
    ref.columns.yaw[ref.slot] = 1.0f;
    ref.columns.health[ref.slot] = -20;
    ref.columns.flags[ref.slot] = 0x00F0;
    ref.columns.state[ref.slot] = 3;
    rig.Refresh();
    rig.Enter(id, 0);
    const std::vector<uint8_t> bytes = rig.Encode(7);
    // The same record written field by field: gap, "complete", every property in full, the terminator.
    std::vector<uint8_t> expected;
    BitWriter writer(expected);
    writer.WriteVarUInt(1);
    writer.WriteBool(true);
    writer.WriteBits(1'000'150, 21); // 1.5 m, 1 cm steps from -10,000 m
    writer.WriteBits(1'000'250, 21);
    writer.WriteBits(999'650, 21);
    writer.WriteBits(163, 10); // 1 radian of 1,024 steps around the circle
    writer.WriteBits(980, 11); // health -20 in -1000..1000
    writer.WriteBits(0x00F0, 16);
    writer.WriteBits(3, 3);
    writer.WriteVarUInt(0);
    writer.Finish();
    TESSERA_CHECK(bytes == expected);
    TESSERA_CHECK(bytes.size() == 14);
}

TESSERA_TEST(stopped_and_confirmed_entity_writes_only_the_terminator) {
    Rig rig;
    const EntityId id = rig.Spawn(1.0f, 2.0f, 3.0f);
    rig.Refresh();
    rig.Enter(id, 0);
    rig.Encode(1);
    rig.conn.Confirm(1);
    TESSERA_CHECK(!rig.conn.IsPending(0) && rig.conn.HasBaseline(0));
    EncodeStats stats;
    const std::vector<uint8_t> bytes = rig.Encode(2, &stats);
    TESSERA_CHECK(stats.records == 0);
    TESSERA_CHECK(bytes.size() == 1); // just the 4-bit terminator, padded to a byte
}

TESSERA_TEST(confirm_promotes_only_the_exact_tick) {
    Rig rig;
    const EntityId id = rig.Spawn(1.0f, 2.0f, 3.0f);
    rig.Refresh();
    rig.Enter(id, 0);
    rig.Encode(5);
    rig.Encode(6); // the same state is sent again, now tagged with tick 6
    rig.conn.Confirm(5);
    TESSERA_CHECK(!rig.conn.HasBaseline(0) && rig.conn.IsPending(0)); // 5 was superseded by the send at 6
    rig.conn.Confirm(6);
    TESSERA_CHECK(rig.conn.HasBaseline(0) && !rig.conn.IsPending(0));
    rig.MoveX(id, 9.0f);
    rig.Refresh();
    EncodeStats stats;
    rig.Encode(7, &stats);
    TESSERA_CHECK(stats.records == 1 &&
                  stats.complete_records == 0); // partial, against the state confirmed at 6
}

TESSERA_TEST(unknown_or_repeated_confirm_has_no_effect) {
    Rig rig;
    const EntityId id = rig.Spawn(1.0f, 2.0f, 3.0f);
    rig.Refresh();
    rig.Enter(id, 0);
    rig.Encode(5);
    rig.conn.Confirm(99); // never sent
    rig.conn.Confirm(4);
    TESSERA_CHECK(!rig.conn.HasBaseline(0) && rig.conn.IsPending(0));
    rig.conn.Confirm(5);
    rig.conn.Confirm(5); // repeated
    TESSERA_CHECK(rig.conn.HasBaseline(0) && !rig.conn.IsPending(0));
}

TESSERA_TEST(returning_to_the_acked_value_with_a_send_outstanding_writes_an_empty_mask_record) {
    Rig rig;
    const EntityId id = rig.Spawn(1.0f, 2.0f, 3.0f);
    rig.Refresh();
    rig.Enter(id, 0);
    TESSERA_CHECK(rig.receiver.Apply(rig.Encode(1), 1) == DecodeStatus::Ok);
    rig.conn.Confirm(1);
    rig.MoveX(id, 5.0f);
    rig.Refresh();
    const std::vector<uint8_t> at_two =
        rig.Encode(2); // the client gets this one, but the confirmation is lost
    TESSERA_CHECK(rig.receiver.Apply(at_two, 2) == DecodeStatus::Ok);
    rig.MoveX(id, 1.0f); // back to the state confirmed at tick 1
    rig.Refresh();
    EncodeStats stats;
    const std::vector<uint8_t> at_three = rig.Encode(3, &stats);
    TESSERA_CHECK(stats.records == 1 && stats.complete_records == 0);
    BitReader reader(at_three);
    uint32_t gap = 0, age = 0;
    bool complete = true;
    TESSERA_CHECK(ReadRecordHead(reader, &gap, &complete, &age));
    TESSERA_CHECK(!complete && age == 2 &&
                  reader.ReadBits(5) == 0); // an empty mask: "same as the confirmed state"
    TESSERA_CHECK(rig.receiver.Apply(at_three, 3) == DecodeStatus::Ok);
    TESSERA_CHECK(rig.ReceiverMatchesStore(id, 0) && rig.receiver.CurrentTick(0) == 3);
}

TESSERA_TEST(baseline_older_than_the_window_forces_a_complete_record) {
    Rig rig(1000, 32);
    const EntityId id = rig.Spawn(1.0f, 2.0f, 3.0f);
    rig.Refresh();
    rig.Enter(id, 0);
    rig.Encode(1);
    rig.conn.Confirm(1);
    rig.MoveX(id, 4.0f);
    rig.Refresh();
    EncodeStats stats;
    rig.Encode(33, &stats); // exactly 32 ticks after the confirmed state: still a delta
    TESSERA_CHECK(stats.records == 1 && stats.complete_records == 0);
    rig.Encode(34, &stats); // 33 ticks: the receiver may have forgotten it, so everything is sent
    TESSERA_CHECK(stats.records == 1 && stats.complete_records == 1);
}

TESSERA_TEST(unconfirmed_entity_is_sent_again_every_tick) {
    Rig rig;
    const EntityId id = rig.Spawn(1.0f, 2.0f, 3.0f);
    rig.Refresh();
    rig.Enter(id, 0);
    for (uint64_t tick = 1; tick <= 4; ++tick) {
        EncodeStats stats;
        rig.Encode(tick, &stats);
        TESSERA_CHECK(stats.records == 1);
    }
}

TESSERA_TEST(entity_without_quantized_state_is_skipped_and_counted) {
    Rig rig;
    const EntityId id = rig.table.Create(); // never flagged as changed
    rig.Enter(id, 0);
    EncodeStats stats;
    rig.Encode(1, &stats);
    TESSERA_CHECK(stats.records == 0 && stats.skipped_unquantized == 1);
    TESSERA_CHECK(rig.conn.IsPending(0)); // it stays pending
    rig.table.MarkDirty(id);
    rig.Refresh();
    rig.Encode(2, &stats);
    TESSERA_CHECK(stats.records == 1 && stats.skipped_unquantized == 0);
}

TESSERA_TEST(reused_compact_id_starts_from_an_empty_baseline) {
    Rig rig;
    const EntityId a = rig.Spawn(1.0f, 2.0f, 3.0f);
    const EntityId b = rig.Spawn(7.0f, 8.0f, 9.0f);
    rig.Refresh();
    rig.Enter(a, 4);
    rig.Encode(1);
    rig.conn.Confirm(1);
    TESSERA_CHECK(rig.conn.HasBaseline(4));
    rig.Leave(a, 4);
    rig.Enter(b, 4); // another entity takes the number
    TESSERA_CHECK(!rig.conn.HasBaseline(4) && rig.conn.IsPending(4));
    EncodeStats stats;
    const std::vector<uint8_t> bytes = rig.Encode(2, &stats);
    TESSERA_CHECK(stats.records == 1 && stats.complete_records == 1);
    TESSERA_CHECK(rig.receiver.Apply(bytes, 2) == DecodeStatus::Ok && rig.ReceiverMatchesStore(b, 4));
}

TESSERA_TEST(receiver_never_regresses_on_an_older_record) {
    Rig rig;
    const EntityId id = rig.Spawn(1.0f, 2.0f, 3.0f);
    rig.Refresh();
    rig.Enter(id, 0);
    const std::vector<uint8_t> t6 = rig.Encode(6);
    rig.conn.Confirm(6);
    rig.MoveX(id, 10.0f);
    rig.Refresh();
    const std::vector<uint8_t> t7 = rig.Encode(7); // against the state confirmed at 6
    rig.MoveX(id, 20.0f);
    rig.Refresh();
    const std::vector<uint8_t> t8 = rig.Encode(8); // still against 6, since 7 was not confirmed
    TESSERA_CHECK(rig.receiver.Apply(t6, 6) == DecodeStatus::Ok);
    TESSERA_CHECK(rig.receiver.Apply(t8, 8) == DecodeStatus::Ok);
    const std::vector<uint32_t> at_eight(rig.receiver.Current(0), rig.receiver.Current(0) + 9);
    TESSERA_CHECK(rig.receiver.Apply(t7, 7) == DecodeStatus::Ok); // arrives late: kept as history only
    TESSERA_CHECK(rig.receiver.CurrentTick(0) == 8);
    TESSERA_CHECK(std::equal(at_eight.begin(), at_eight.end(), rig.receiver.Current(0)));
    TESSERA_CHECK(rig.receiver.HistorySize(0) == 3);
}

TESSERA_TEST(receiver_rejects_a_record_with_an_unknown_baseline_age) {
    Rig rig;
    const EntityId id = rig.Spawn(1.0f, 2.0f, 3.0f);
    rig.Refresh();
    rig.Enter(id, 0);
    TESSERA_CHECK(rig.receiver.Apply(rig.Encode(1), 1) == DecodeStatus::Ok);
    rig.conn.Confirm(1);
    rig.MoveX(id, 6.0f);
    rig.Refresh();
    const std::vector<uint8_t> bytes = rig.Encode(2);
    const std::vector<uint32_t> before(rig.receiver.Current(0), rig.receiver.Current(0) + 9);
    size_t applied = 99;
    TESSERA_CHECK(rig.receiver.Apply(bytes, 20, &applied) ==
                  DecodeStatus::MissingBaseline); // base would be tick 19
    TESSERA_CHECK(applied == 0);
    TESSERA_CHECK(std::equal(before.begin(), before.end(), rig.receiver.Current(0)) &&
                  rig.receiver.CurrentTick(0) == 1);
}

TESSERA_TEST(receiver_rejects_an_unknown_compact_id) {
    Rig rig;
    const EntityId id = rig.Spawn(1.0f, 2.0f, 3.0f);
    rig.Refresh();
    rig.conn.ApplyEvents(
        std::vector<VisibilityEvent>{{VisibilityEvent::Kind::Enter, id, 0}}); // the receiver is not told
    const std::vector<uint8_t> bytes = rig.Encode(1);
    size_t applied = 99;
    TESSERA_CHECK(rig.receiver.Apply(bytes, 1, &applied) == DecodeStatus::UnknownEntity && applied == 0);
    TESSERA_CHECK(!rig.receiver.OnEnter(5000, id));                       // an id beyond the maximum
    TESSERA_CHECK(!rig.receiver.OnEnter(1, MakeEntity(kUnknownType, 1))); // a type without schema
}

TESSERA_TEST(receiver_prunes_history_older_than_the_window) {
    Rig rig(1000, 32);
    const EntityId id = rig.Spawn(1.0f, 2.0f, 3.0f);
    rig.Refresh();
    rig.Enter(id, 0);
    for (uint64_t tick = 1; tick <= 100; ++tick) {
        rig.MoveX(id, static_cast<float>(tick));
        rig.Refresh();
        TESSERA_CHECK(rig.receiver.Apply(rig.Encode(tick), tick) == DecodeStatus::Ok);
    }
    TESSERA_CHECK(rig.receiver.HistorySize(0) <= 33);
    TESSERA_CHECK(rig.receiver.CurrentTick(0) == 100);
}

TESSERA_TEST(receiver_rejects_truncated_buffers_and_keeps_its_state) {
    Rig rig;
    std::vector<EntityId> ids;
    for (int i = 0; i < 20; ++i) {
        ids.push_back(rig.Spawn(static_cast<float>(i), 2.0f, 3.0f));
        rig.Enter(ids.back(), static_cast<uint32_t>(i));
    }
    rig.Refresh();
    const std::vector<uint8_t> full = rig.Encode(1);
    for (size_t cut = 0; cut < full.size(); ++cut) {
        Rig other; // a fresh receiver that knows the same entities
        for (int i = 0; i < 20; ++i) {
            other.receiver.OnEnter(static_cast<uint32_t>(i), ids[static_cast<size_t>(i)]);
        }
        size_t applied = 0;
        const DecodeStatus status =
            other.receiver.Apply(std::span<const uint8_t>(full.data(), cut), 1, &applied);
        TESSERA_CHECK(status != DecodeStatus::Ok); // the terminator is missing
        for (uint32_t i = 0; i < 20; ++i) {
            const uint32_t* words = other.receiver.Current(i);
            if (i < applied) {
                const uint32_t* expected = rig.store.Find(ids[i]);
                TESSERA_CHECK(words != nullptr && expected != nullptr &&
                              std::equal(expected, expected + 9, words));
            } else {
                TESSERA_CHECK(words == nullptr); // a record is applied whole or not at all
            }
        }
    }
}

TESSERA_TEST(receiver_survives_corrupt_buffers) {
    Rig rig;
    std::vector<EntityId> ids;
    for (int i = 0; i < 20; ++i) {
        ids.push_back(rig.Spawn(static_cast<float>(i), 2.0f, 3.0f));
        rig.Enter(ids.back(), static_cast<uint32_t>(i));
    }
    rig.Refresh();
    const std::vector<uint8_t> clean = rig.Encode(1);
    std::mt19937 random(5);
    for (int round = 0; round < 1000; ++round) {
        std::vector<uint8_t> noisy = clean;
        const int flips = 1 + static_cast<int>(random() % 8);
        for (int f = 0; f < flips; ++f) {
            noisy[random() % noisy.size()] ^= static_cast<uint8_t>(1u << (random() % 8));
        }
        Rig other;
        for (int i = 0; i < 20; ++i) {
            other.receiver.OnEnter(static_cast<uint32_t>(i), ids[static_cast<size_t>(i)]);
        }
        size_t applied = 0;
        other.receiver.Apply(noisy, 1, &applied); // must not crash, whatever the bits say
        TESSERA_CHECK(applied <= 20);
    }
}
