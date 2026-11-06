// SPDX-License-Identifier: Apache-2.0
#include "network_test_support.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

using namespace network_test;

namespace {

// Connects a harness through the handshake, so a test can start from a client that is ready.
void Handshake(ClientHarness& h, uint32_t connection_id = 3, uint32_t max_visible = 1000) {
    h.Step();
    h.Step();
    h.Accept(connection_id, max_visible);
    h.Step();
}

} // namespace

TESSERA_TEST(client_sends_its_hello_when_connected) {
    ClientHarness h;
    TESSERA_CHECK(h.client.State() == ClientState::Connecting);
    h.Step();
    TESSERA_CHECK(h.client.State() == ClientState::Handshaking);
    h.Step();
    const std::vector<ClientHello> hellos = h.Hellos();
    TESSERA_CHECK(hellos.size() == 1);
    TESSERA_CHECK(hellos.size() == 1 && hellos[0].version == kProtocolVersion &&
                  hellos[0].schema_hash == h.schema.Hash());
}

TESSERA_TEST(accepted_hello_connects_and_exposes_the_connection_id) {
    ClientHarness h;
    Handshake(h, 7);
    TESSERA_CHECK(h.client.State() == ClientState::Connected);
    TESSERA_CHECK(h.client.ConnectionId() == 7);
    TESSERA_CHECK(h.client.VisibleCount() == 0);
}

TESSERA_TEST(each_rejection_reaches_the_client_with_its_reason) {
    for (const RejectReason reason :
         {RejectReason::VersionMismatch, RejectReason::SchemaMismatch, RejectReason::ServerFull}) {
        ClientHarness h;
        h.Step();
        h.Step();
        h.Reject(reason);
        h.Step();
        TESSERA_CHECK(h.client.State() == ClientState::Rejected);
        TESSERA_CHECK(h.client.Rejection() == reason);
        const size_t sent = h.MessagesFromClient();
        for (int i = 0; i < 5; ++i) {
            h.Step();
        }
        TESSERA_CHECK(h.MessagesFromClient() == sent); // nothing more is sent
        TESSERA_CHECK(h.client.State() ==
                      ClientState::Rejected); // and the server's disconnect does not change that
    }
}

TESSERA_TEST(event_batches_in_sequence_make_entities_visible) {
    ClientHarness h;
    Handshake(h);
    const EntityId a = MakeEntity(kMobType, 10), b = MakeEntity(kMobType, 20);
    h.SendBatch(1, 5, {Enter(a, 0), Enter(b, 1)});
    h.SendBatch(2, 6, {Leave(a, 0)});
    h.Step();
    TESSERA_CHECK(h.client.State() == ClientState::Connected);
    TESSERA_CHECK(h.client.VisibleCount() == 1);
    h.SendBatch(3, 7, {Destroy(b, 1), Enter(a, 2)});
    h.Step();
    TESSERA_CHECK(h.client.VisibleCount() == 1);
}

TESSERA_TEST(a_batch_out_of_sequence_is_a_protocol_error_and_disconnects) {
    struct Case {
        std::vector<uint64_t> numbers;
        size_t visible_before_error;
    };
    const std::vector<std::vector<uint64_t>> cases = {{1, 3}, {1, 1}, {2}, {0}, {1, 2, 2}};
    for (const std::vector<uint64_t>& numbers : cases) {
        ClientHarness h;
        Handshake(h);
        uint32_t next = 0;
        for (const uint64_t number : numbers) {
            h.SendBatch(number, number, {Enter(MakeEntity(kMobType, 100 + next), next)});
            ++next;
        }
        h.Step();
        TESSERA_CHECK(h.client.State() == ClientState::Disconnected);
        TESSERA_CHECK(h.client.Stats().protocol_errors == 1);
        TESSERA_CHECK(h.client.VisibleCount() == 0); // a connection that broke the rules keeps no state
    }
}

TESSERA_TEST(a_batch_with_an_unknown_type_or_a_compact_id_out_of_range_is_a_protocol_error) {
    {
        ClientHarness h;
        Handshake(h);
        h.SendBatch(1, 1, {Enter(MakeEntity(kUnknownType, 4), 0)}); // no schema for this type
        h.Step();
        TESSERA_CHECK(h.client.State() == ClientState::Disconnected && h.client.Stats().protocol_errors == 1);
    }
    {
        ClientHarness h;
        Handshake(h, 3, 10);
        h.SendBatch(1, 1, {Enter(MakeEntity(kMobType, 4), 10)}); // ids go from 0 to 9
        h.Step();
        TESSERA_CHECK(h.client.State() == ClientState::Disconnected && h.client.Stats().protocol_errors == 1);
    }
    {
        ClientHarness h;
        Handshake(h, 3, 10);
        h.SendBatch(1, 1, {Leave(MakeEntity(kMobType, 4), 99)}); // a leave for an id that cannot exist
        h.Step();
        TESSERA_CHECK(h.client.State() == ClientState::Disconnected && h.client.Stats().protocol_errors == 1);
    }
}

TESSERA_TEST(messages_before_the_server_hello_are_ignored_and_counted) {
    ClientHarness h;
    h.Step();
    h.Step();
    h.SendBatch(1, 1, {Enter(MakeEntity(kMobType, 4), 0)});
    h.Step();
    TESSERA_CHECK(h.client.State() == ClientState::Handshaking);
    TESSERA_CHECK(h.client.Stats().messages_ignored == 1 && h.client.Stats().protocol_errors == 0);
    h.Accept();
    h.Step();
    TESSERA_CHECK(h.client.State() == ClientState::Connected && h.client.VisibleCount() == 0);
}

TESSERA_TEST(a_second_server_hello_is_ignored) {
    ClientHarness h;
    Handshake(h, 3);
    h.Accept(9);
    h.Step();
    TESSERA_CHECK(h.client.State() == ClientState::Connected && h.client.ConnectionId() == 3);
    TESSERA_CHECK(h.client.Stats().messages_ignored == 1);
}

TESSERA_TEST(a_server_hello_with_absurd_limits_is_a_protocol_error) {
    struct Hello {
        uint32_t tick_rate, max_visible, baseline_age, version;
    };
    const std::vector<Hello> bad = {{30, 0, 32, kProtocolVersion},   {30, 10'000'000, 32, kProtocolVersion},
                                    {30, 1000, 0, kProtocolVersion}, {30, 1000, 1000, kProtocolVersion},
                                    {0, 1000, 32, kProtocolVersion}, {30, 1000, 32, kProtocolVersion + 1}};
    for (const Hello& hello : bad) {
        ClientHarness h;
        h.Step();
        h.Step();
        h.SendHello(ServerHello{true, RejectReason::None, hello.version, 1, hello.tick_rate,
                                hello.max_visible, hello.baseline_age});
        h.Step();
        TESSERA_CHECK(h.client.State() == ClientState::Disconnected);
        TESSERA_CHECK(h.client.Stats().protocol_errors == 1);
    }
}

TESSERA_TEST(garbage_on_the_unreliable_channel_is_dropped_and_counted) {
    ClientHarness h;
    Handshake(h);
    h.SendBatch(1, 1, {Enter(MakeEntity(kMobType, 4), 0)});
    h.Step();
    std::mt19937 random(11);
    uint64_t sent = 0;
    for (int i = 0; i < 200; ++i) {
        std::vector<uint8_t> bytes(random() % 20);
        for (uint8_t& b : bytes) {
            b = static_cast<uint8_t>(random());
        }
        h.Send(Channel::Unreliable, bytes);
        ++sent;
    }
    std::vector<uint8_t> truncated;
    Encode(EventBatch{2, 2, {Enter(MakeEntity(kMobType, 5), 1)}}, truncated);
    truncated.resize(truncated.size() / 2);
    h.Send(Channel::Unreliable, truncated); // a half message of a kind that belongs on the reliable channel
    h.Step();
    TESSERA_CHECK(h.client.State() == ClientState::Connected);
    TESSERA_CHECK(h.client.VisibleCount() == 1);
    TESSERA_CHECK(h.client.Stats().protocol_errors == 0);
    TESSERA_CHECK(h.client.Stats().malformed_messages + h.client.Stats().messages_ignored +
                      h.client.Stats().buffers_refused + h.client.Stats().buffers_held >
                  sent / 2);
}

TESSERA_TEST(an_event_batch_on_the_unreliable_channel_is_dropped_not_applied) {
    ClientHarness h;
    Handshake(h);
    std::vector<uint8_t> bytes;
    Encode(EventBatch{1, 1, {Enter(MakeEntity(kMobType, 4), 0)}}, bytes);
    h.Send(Channel::Unreliable, bytes); // events travel on the reliable channel only
    h.Step();
    TESSERA_CHECK(h.client.VisibleCount() == 0 && h.client.State() == ClientState::Connected);
    TESSERA_CHECK(h.client.Stats().malformed_messages == 1);
}

TESSERA_TEST(garbage_on_the_reliable_channel_is_a_protocol_error) {
    ClientHarness h;
    Handshake(h);
    h.Send(Channel::ReliableOrdered, std::vector<uint8_t>{0xFF, 0xFF, 0xFF});
    h.Step();
    TESSERA_CHECK(h.client.State() == ClientState::Disconnected && h.client.Stats().protocol_errors == 1);
}

TESSERA_TEST(server_disconnect_moves_to_disconnected_and_clears_the_visible_set) {
    ClientHarness h;
    Handshake(h);
    h.SendBatch(1, 1, {Enter(MakeEntity(kMobType, 4), 0), Enter(MakeEntity(kMobType, 5), 1)});
    h.Step();
    TESSERA_CHECK(h.client.VisibleCount() == 2);
    h.network.Server().Disconnect(h.connection);
    h.Step();
    h.Step();
    TESSERA_CHECK(h.client.State() == ClientState::Disconnected);
    TESSERA_CHECK(h.client.VisibleCount() == 0);
    TESSERA_CHECK(h.client.Stats().protocol_errors == 0);
}

TESSERA_TEST(a_client_that_is_told_to_connect_twice_sends_one_hello) {
    ClientHarness h;
    for (int i = 0; i < 6; ++i) {
        h.Step();
    }
    TESSERA_CHECK(h.Hellos().size() == 1);
}

// ---- State buffers and confirmations ----------------------------------------------------------------

namespace {

// A connected client, a source of real state buffers, and one entity that has entered through batch 1 (not
// yet sent: the test decides when).
struct Scene {
    ClientHarness h;
    ReplicationSource source{h.schema};
    EntityId entity = source.Spawn(1.0f, 2.0f, 3.0f);
    std::vector<VisibilityEvent> enter{Enter(entity, 0)};

    Scene() {
        Handshake(h);
        source.Show(enter);
    }
};

} // namespace

TESSERA_TEST(state_buffer_is_applied_and_acked) {
    Scene s;
    s.h.SendBatch(1, 1, s.enter);
    s.h.SendStateBuffer(1, 1, s.source.Write(1));
    s.h.Step();
    s.h.Step(); // the ack reaches the server end
    TESSERA_CHECK(SameWords(s.h.client.WordsOf(s.entity), s.source.Words(s.entity)));
    TESSERA_CHECK(s.h.client.WordsOf(s.entity) != nullptr);
    TESSERA_CHECK(s.h.client.LatestTick() == 1 && s.h.client.Stats().buffers_applied == 1);
    const std::vector<Ack> acks = s.h.Acks();
    TESSERA_CHECK(acks.size() == 1 && acks[0].latest_tick == 1 && acks[0].mask == 0);
}

TESSERA_TEST(buffer_that_arrives_before_its_event_batches_is_held_and_applied_when_they_come) {
    Scene s;
    s.h.SendStateBuffer(1, 1, s.source.Write(1)); // cites batch 1, which has not arrived
    s.h.Step();
    s.h.Step();
    TESSERA_CHECK(s.h.client.WordsOf(s.entity) == nullptr && s.h.client.VisibleCount() == 0);
    TESSERA_CHECK(s.h.Acks().empty());
    TESSERA_CHECK(s.h.client.Stats().buffers_held == 1 && s.h.client.Stats().buffers_applied == 0);
    s.h.SendBatch(1, 1, s.enter);
    s.h.Step();
    s.h.Step();
    TESSERA_CHECK(SameWords(s.h.client.WordsOf(s.entity), s.source.Words(s.entity)));
    TESSERA_CHECK(s.h.client.WordsOf(s.entity) != nullptr && s.h.Acks().size() == 1);
}

TESSERA_TEST(buffers_are_not_decoded_before_their_batches_even_when_duplicated_or_reordered) {
    Scene s;
    const std::vector<uint8_t> at_one = s.source.Write(1);
    s.source.MoveX(s.entity, 50.0f);
    const std::vector<uint8_t> at_two = s.source.Write(2); // the entity moved
    s.h.SendStateBuffer(2, 1, at_two);
    s.h.SendStateBuffer(2, 1, at_two); // a duplicate
    s.h.SendStateBuffer(1, 1, at_one); // and the older one last
    s.h.Step();
    TESSERA_CHECK(s.h.client.WordsOf(s.entity) == nullptr && s.h.client.Stats().buffers_applied == 0);
    TESSERA_CHECK(s.h.client.Stats().buffers_held == 3);
    s.h.SendBatch(1, 1, s.enter);
    s.h.Step();
    s.h.Step();
    TESSERA_CHECK(s.h.client.Stats().buffers_applied == 2); // each tick once; the duplicate was skipped
    TESSERA_CHECK(s.h.client.LatestTick() == 2);
    TESSERA_CHECK(
        SameWords(s.h.client.WordsOf(s.entity), s.source.Words(s.entity))); // the state did not go back
    TESSERA_CHECK(s.h.client.WordsOf(s.entity) != nullptr);
}

TESSERA_TEST(refused_buffers_are_not_acked) {
    Scene s;
    s.h.SendBatch(1, 5, s.enter);                 // the client learns of the entity at tick 5
    s.h.SendStateBuffer(3, 1, s.source.Write(3)); // a buffer from before that, naming its compact id
    s.h.Step();
    s.h.Step();
    TESSERA_CHECK(s.h.client.Stats().buffers_refused == 1 && s.h.client.Stats().buffers_applied == 0);
    TESSERA_CHECK(s.h.Acks().empty());
    TESSERA_CHECK(s.h.client.WordsOf(s.entity) == nullptr);
}

TESSERA_TEST(ack_carries_the_latest_tick_and_the_mask_of_the_previous_applied_ticks) {
    Scene s;
    s.h.SendBatch(1, 4, s.enter);
    s.h.Step();
    for (const uint64_t tick : {5, 6, 8}) {
        s.source.MoveX(s.entity, static_cast<float>(tick));
        s.h.SendStateBuffer(tick, 1, s.source.Write(tick));
        s.h.Step();
    }
    s.h.Step();
    const std::vector<Ack> acks = s.h.Acks();
    TESSERA_CHECK(acks.size() == 3);
    TESSERA_CHECK(acks.back().latest_tick == 8 && acks.back().mask == 0b110); // 7 missing, 6 and 5 applied
}

TESSERA_TEST(one_ack_per_poll_even_with_several_buffers) {
    Scene s;
    s.h.SendBatch(1, 1, s.enter);
    for (const uint64_t tick : {1, 2, 3}) {
        s.source.MoveX(s.entity, static_cast<float>(tick));
        s.h.SendStateBuffer(tick, 1, s.source.Write(tick));
    }
    s.h.Step();
    s.h.Step();
    const std::vector<Ack> acks = s.h.Acks();
    TESSERA_CHECK(acks.size() == 1 && acks[0].latest_tick == 3 && acks[0].mask == 0b11);
    TESSERA_CHECK(s.h.client.Stats().acks_sent == 1 && s.h.client.Stats().buffers_applied == 3);
}

TESSERA_TEST(no_ack_when_nothing_was_applied) {
    Scene s;
    s.h.SendBatch(1, 1, s.enter);
    for (int i = 0; i < 5; ++i) {
        s.h.Step();
    }
    TESSERA_CHECK(s.h.Acks().empty() && s.h.client.Stats().acks_sent == 0);
}

TESSERA_TEST(held_buffers_are_capped_at_64_and_the_oldest_are_dropped) {
    Scene s;
    const std::vector<uint8_t> buffer =
        s.source.Write(1); // complete records: the same bytes serve every tick
    for (uint64_t tick = 1; tick <= 70; ++tick) {
        s.h.SendStateBuffer(tick, 1, buffer);
    }
    s.h.Step();
    TESSERA_CHECK(s.h.client.Stats().buffers_held == 70 && s.h.client.Stats().buffers_dropped == 6);
    s.h.SendBatch(1, 1, s.enter);
    s.h.Step();
    TESSERA_CHECK(s.h.client.Stats().buffers_applied == 64); // ticks 7 to 70 survived
    TESSERA_CHECK(s.h.client.LatestTick() == 70);
}

TESSERA_TEST(a_held_buffer_for_a_tick_that_was_already_applied_is_discarded_on_release) {
    Scene s;
    s.h.SendBatch(1, 1, s.enter);
    s.h.SendStateBuffer(5, 1, s.source.Write(5));
    s.h.Step();
    s.h.Step();
    TESSERA_CHECK(s.h.client.Stats().buffers_applied == 1 && s.h.Acks().size() == 1);
    s.h.SendStateBuffer(5, 2, s.source.Write(5)); // a duplicate, citing a batch that has not come yet
    s.h.Step();
    s.h.SendBatch(2, 6, {});
    s.h.Step();
    s.h.Step();
    TESSERA_CHECK(s.h.client.Stats().buffers_applied == 1 && s.h.Acks().size() == 1); // nothing to do for it
}

TESSERA_TEST(for_each_entity_visits_exactly_the_visible_entities_that_have_state) {
    Scene s;
    const EntityId second = s.source.Spawn(9.0f, 9.0f, 9.0f);
    s.source.Show({Enter(second, 1)});
    s.h.SendBatch(1, 1, {Enter(s.entity, 0), Enter(second, 1)});
    s.h.Step();
    size_t visited = 0;
    s.h.client.ForEachEntity([&](EntityId, const uint32_t*) { ++visited; });
    TESSERA_CHECK(s.h.client.VisibleCount() == 2 && visited == 0); // they entered, but no state has come yet
    s.h.SendStateBuffer(1, 1, s.source.Write(1));
    s.h.Step();
    std::vector<uint64_t> seen;
    s.h.client.ForEachEntity([&](EntityId id, const uint32_t* words) {
        seen.push_back(id.raw());
        TESSERA_CHECK(SameWords(words, s.source.Words(id)));
    });
    std::sort(seen.begin(), seen.end());
    TESSERA_CHECK(seen.size() == 2 && seen[0] == std::min(s.entity.raw(), second.raw()));
    s.h.SendBatch(2, 2, {Leave(second, 1)});
    s.h.Step();
    visited = 0;
    s.h.client.ForEachEntity([&](EntityId id, const uint32_t*) { visited += id == s.entity ? 1 : 100; });
    TESSERA_CHECK(visited == 1);
}

TESSERA_TEST(malformed_state_buffers_are_dropped_without_changing_the_state) {
    Scene s;
    s.h.SendBatch(1, 1, s.enter);
    s.h.SendStateBuffer(1, 1, s.source.Write(1));
    s.h.Step();
    const std::vector<uint32_t> before(s.h.client.WordsOf(s.entity), s.h.client.WordsOf(s.entity) + 9);
    s.h.Send(Channel::Unreliable, std::vector<uint8_t>{4}); // a header that stops after its kind byte
    s.h.SendStateBuffer(
        2, 1, std::vector<uint8_t>{0xFF, 0xFF, 0xFF, 0xFF}); // a header, and a payload that is nonsense
    s.h.Step();
    TESSERA_CHECK(s.h.client.Stats().malformed_messages == 1 && s.h.client.Stats().buffers_refused == 1);
    TESSERA_CHECK(s.h.client.State() == ClientState::Connected && s.h.client.Stats().protocol_errors == 0);
    TESSERA_CHECK(SameWords(s.h.client.WordsOf(s.entity), before.data()));
    TESSERA_CHECK(s.h.client.LatestTick() == 1);
}

TESSERA_TEST(a_state_buffer_before_the_hello_is_ignored) {
    ClientHarness h;
    h.Step();
    h.Step();
    h.SendStateBuffer(1, 0, std::vector<uint8_t>{0});
    h.Step();
    TESSERA_CHECK(h.client.Stats().messages_ignored == 1 && h.client.State() == ClientState::Handshaking);
}

TESSERA_TEST(latest_tick_is_the_newest_applied_even_if_an_older_one_arrives_later) {
    Scene s;
    s.h.SendBatch(1, 1, s.enter);
    s.h.SendStateBuffer(9, 1, s.source.Write(9));
    s.h.Step();
    s.h.SendStateBuffer(4, 1, s.source.Write(4)); // late
    s.h.Step();
    TESSERA_CHECK(s.h.client.LatestTick() == 9 && s.h.client.Stats().buffers_applied == 2);
}
