// SPDX-License-Identifier: Apache-2.0
// The server's side of player input: what reaches the game, once and in order, and what is dropped.
#include "server_test_support.hpp"
#include "tessera/net/replication/bit_stream.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

using namespace network_test;

namespace {

void Settle(ServerHarness& h) {
    h.Step();
    h.Step();
    h.Step();
}

std::vector<uint32_t> Sequences(const ServerHarness& h, ConnectionId player) {
    std::vector<uint32_t> found;
    for (const ServerHarness::DeliveredInput& input : h.inputs) {
        if (input.player == player) {
            found.push_back(input.sequence);
        }
    }
    return found;
}

ServerConfig LimitedConfig(uint32_t max_inputs_per_tick) {
    ServerConfig config = SmallServerConfig();
    config.max_inputs_per_tick = max_inputs_per_tick;
    return config;
}

using Sequence = std::vector<uint32_t>;

} // namespace

TESSERA_TEST(input_from_a_greeted_player_reaches_the_handler_once_with_its_sequence_and_bytes) {
    ServerHarness h;
    FakeClient& client = h.ConnectAndGreet();
    client.SendInput(Channel::Unreliable, {7});
    Settle(h);
    TESSERA_CHECK(h.inputs.size() == 1);
    if (h.inputs.size() == 1) {
        TESSERA_CHECK(h.inputs[0].player == 0 && h.inputs[0].sequence == 7);
        TESSERA_CHECK((h.inputs[0].bytes == std::vector<uint8_t>{7}));
    }
    TESSERA_CHECK(h.server.Stats().inputs_delivered == 1);
}

TESSERA_TEST(input_before_the_hello_is_ignored_and_counted) {
    ServerHarness h;
    FakeClient& client = h.AddClient();
    h.Step();
    client.SendInput(Channel::Unreliable, {1});
    Settle(h);
    TESSERA_CHECK(h.inputs.empty());
    TESSERA_CHECK(h.server.Stats().messages_ignored == 1 && h.server.Stats().inputs_delivered == 0);
    client.SendHello(kProtocolVersion, h.schema.Hash());
    Settle(h);
    TESSERA_CHECK(h.server.ConnectionCount() == 1);
    client.SendInput(Channel::Unreliable, {1});
    Settle(h);
    TESSERA_CHECK(Sequences(h, 0) == Sequence{1});
}

TESSERA_TEST(a_repeated_entry_is_delivered_once) {
    ServerHarness h;
    FakeClient& client = h.ConnectAndGreet();
    client.SendInput(Channel::Unreliable, {1, 2, 3, 4});
    h.Step();
    client.SendInput(Channel::Unreliable, {2, 3, 4, 5});
    h.Step();
    client.SendInput(Channel::Unreliable, {3, 4, 5, 6});
    Settle(h);
    TESSERA_CHECK((Sequences(h, 0) == Sequence{1, 2, 3, 4, 5, 6}));
    TESSERA_CHECK(h.server.Stats().inputs_delivered == 6 && h.server.Stats().inputs_repeated == 6);
}

TESSERA_TEST(entries_are_delivered_in_sequence_order_even_when_messages_arrive_out_of_order) {
    ServerHarness h;
    FakeClient& client = h.ConnectAndGreet();
    client.SendInput(Channel::Unreliable, {5, 6});
    h.Step();
    client.SendInput(Channel::Unreliable, {3, 4});
    Settle(h);
    TESSERA_CHECK((Sequences(h, 0) == Sequence{5, 6}));
    TESSERA_CHECK(h.server.Stats().inputs_old == 2);
}

TESSERA_TEST(an_entry_older_than_the_newest_delivered_is_ignored_and_counted_old) {
    ServerHarness h;
    FakeClient& client = h.ConnectAndGreet();
    client.SendInput(Channel::Unreliable, {10});
    h.Step();
    client.SendInput(Channel::Unreliable, {7});
    Settle(h);
    TESSERA_CHECK((Sequences(h, 0) == Sequence{10}));
    TESSERA_CHECK(h.server.Stats().inputs_old == 1 && h.server.Stats().inputs_repeated == 0);
}

TESSERA_TEST(a_lost_message_is_covered_by_the_repeat_in_the_next_one) {
    ServerHarness h;
    FakeClient& client = h.ConnectAndGreet();
    client.SendInput(Channel::Unreliable, {1});
    h.Step();
    // The message {1, 2} was lost on the way.
    client.SendInput(Channel::Unreliable, {1, 2, 3});
    Settle(h);
    TESSERA_CHECK((Sequences(h, 0) == Sequence{1, 2, 3}));
    TESSERA_CHECK(h.server.Stats().inputs_delivered == 3 && h.server.Stats().inputs_repeated == 1);
}

TESSERA_TEST(inputs_above_the_per_tick_limit_are_dropped_counted_and_the_server_goes_on) {
    ServerHarness h(LimitedConfig(3));
    FakeClient& client = h.ConnectAndGreet();
    client.SendInput(Channel::Unreliable, {1, 2, 3, 4});
    client.SendInput(Channel::Unreliable, {5, 6, 7, 8});
    Settle(h);
    TESSERA_CHECK((Sequences(h, 0) == Sequence{1, 2, 3}));
    TESSERA_CHECK(h.server.Stats().inputs_dropped == 5);
    TESSERA_CHECK(h.server.ConnectionCount() == 1 && h.server.Stats().protocol_errors == 0);
}

TESSERA_TEST(dropped_entries_are_delivered_in_order_by_the_next_message_that_repeats_them) {
    ServerHarness h(LimitedConfig(3));
    FakeClient& client = h.ConnectAndGreet();
    client.SendInput(Channel::Unreliable, {1, 2, 3, 4});
    client.SendInput(Channel::Unreliable, {5, 6, 7, 8});
    h.Step();
    client.SendInput(Channel::Unreliable, {4, 5, 6, 7});
    Settle(h);
    TESSERA_CHECK((Sequences(h, 0) == Sequence{1, 2, 3, 4, 5, 6}));
    TESSERA_CHECK(h.server.Stats().inputs_repeated == 0);
}

TESSERA_TEST(the_limit_counts_each_player_separately) {
    ServerHarness h(LimitedConfig(3));
    FakeClient& a = h.ConnectAndGreet();
    FakeClient& b = h.ConnectAndGreet();
    a.SendInput(Channel::Unreliable, {1, 2, 3});
    b.SendInput(Channel::Unreliable, {1, 2, 3});
    Settle(h);
    TESSERA_CHECK((Sequences(h, 0) == Sequence{1, 2, 3}));
    TESSERA_CHECK((Sequences(h, 1) == Sequence{1, 2, 3}));
    TESSERA_CHECK(h.server.Stats().inputs_dropped == 0);
}

TESSERA_TEST(a_sequence_that_wraps_around_does_not_stall_the_player) {
    ServerHarness h;
    FakeClient& client = h.ConnectAndGreet();
    for (const uint32_t sequence : {1u, 0x70000001u, 0xE0000001u, 0x50000001u, 0x50000002u}) {
        client.SendInput(Channel::Unreliable, {sequence});
        h.Step();
    }
    TESSERA_CHECK((Sequences(h, 0) == Sequence{1, 0x70000001u, 0xE0000001u, 0x50000001u, 0x50000002u}));
    client.SendInput(Channel::Unreliable, {0x50000001u});
    h.Step();
    client.SendInput(Channel::Unreliable, {0x50000000u});
    Settle(h);
    TESSERA_CHECK(Sequences(h, 0).size() == 5);
    TESSERA_CHECK(h.server.Stats().inputs_repeated == 1 && h.server.Stats().inputs_old == 1);
}

TESSERA_TEST(a_jump_far_ahead_only_blocks_that_player) {
    ServerHarness h;
    FakeClient& a = h.ConnectAndGreet();
    FakeClient& b = h.ConnectAndGreet();
    a.SendInput(Channel::Unreliable, {1});
    h.Step();
    a.SendInput(Channel::Unreliable, {0x7FFFFFFFu});
    h.Step();
    TESSERA_CHECK((Sequences(h, 0) == Sequence{1, 0x7FFFFFFFu}));
    a.SendInput(Channel::Unreliable, {2});
    h.Step();
    a.SendInput(Channel::Unreliable, {3});
    h.Step();
    TESSERA_CHECK(Sequences(h, 0).size() == 2 && h.server.Stats().inputs_old == 2);
    for (const uint32_t sequence : {1u, 2u, 3u}) {
        b.SendInput(Channel::Unreliable, {sequence});
        h.Step();
    }
    Settle(h);
    TESSERA_CHECK((Sequences(h, 1) == Sequence{1, 2, 3}));
}

TESSERA_TEST(input_on_the_reliable_channel_is_a_protocol_error) {
    ServerHarness h;
    FakeClient& client = h.ConnectAndGreet();
    client.SendInput(Channel::ReliableOrdered, {1});
    Settle(h);
    client.Collect();
    TESSERA_CHECK(h.server.Stats().protocol_errors == 1);
    TESSERA_CHECK(client.disconnected && h.server.ConnectionCount() == 0);
    TESSERA_CHECK((h.disconnected == std::vector<ConnectionId>{0}));
    TESSERA_CHECK(h.inputs.empty());
}

TESSERA_TEST(a_malformed_input_is_counted_and_dropped) {
    ServerHarness h;
    FakeClient& client = h.ConnectAndGreet();
    client.Send(Channel::Unreliable, std::vector<uint8_t>{6, 0xFF, 0xFF});
    std::vector<uint8_t> five;
    BitWriter writer(five);
    writer.WriteBits(static_cast<uint64_t>(MessageKind::Input), 8);
    writer.WriteVarUInt(10);
    writer.WriteVarUInt(5); // five entries: more than a message may hold
    writer.Finish();
    client.Send(Channel::Unreliable, five);
    Settle(h);
    TESSERA_CHECK(h.server.Stats().malformed_messages == 2);
    TESSERA_CHECK(h.server.ConnectionCount() == 1 && h.inputs.empty());
}

TESSERA_TEST(input_never_arrives_after_the_disconnect_callback) {
    ServerHarness h;
    FakeClient& client = h.ConnectAndGreet();
    client.SendInput(Channel::Unreliable, {1});
    client.Send(Channel::ReliableOrdered,
                std::vector<uint8_t>{0xFF}); // not a message: the server drops the player
    client.SendInput(Channel::Unreliable, {2});
    Settle(h);
    TESSERA_CHECK((h.order == std::vector<std::string>{"connected:0", "input:0:1", "disconnected:0"}));
    TESSERA_CHECK(h.server.Stats().messages_ignored == 1);
}

TESSERA_TEST(a_player_in_a_reused_slot_starts_with_a_fresh_sequence) {
    ServerHarness h;
    FakeClient& first = h.ConnectAndGreet();
    first.SendInput(Channel::Unreliable, {100});
    Settle(h);
    first.transport->Disconnect(0);
    Settle(h);
    TESSERA_CHECK((h.disconnected == std::vector<ConnectionId>{0}));
    FakeClient& second = h.ConnectAndGreet();
    TESSERA_CHECK((h.connected == std::vector<ConnectionId>{0, 0}));
    second.SendInput(Channel::Unreliable, {1});
    Settle(h);
    TESSERA_CHECK((Sequences(h, 0) == Sequence{100, 1}));
}

TESSERA_TEST(two_players_keep_their_own_sequences) {
    ServerHarness h;
    FakeClient& a = h.ConnectAndGreet();
    FakeClient& b = h.ConnectAndGreet();
    a.SendInput(Channel::Unreliable, {1, 2});
    b.SendInput(Channel::Unreliable, {1, 2});
    h.Step();
    a.SendInput(Channel::Unreliable, {1, 2});
    b.SendInput(Channel::Unreliable, {1, 2});
    Settle(h);
    TESSERA_CHECK(h.inputs.size() == 4);
    TESSERA_CHECK((Sequences(h, 0) == Sequence{1, 2}) && (Sequences(h, 1) == Sequence{1, 2}));
}

TESSERA_TEST(input_without_a_handler_is_counted_and_nothing_breaks) {
    ServerHarness h;
    h.server.SetInputHandler({});
    FakeClient& client = h.ConnectAndGreet();
    client.SendInput(Channel::Unreliable, {1});
    Settle(h);
    TESSERA_CHECK(h.server.Stats().inputs_delivered == 1 && h.inputs.empty());
    TESSERA_CHECK(h.server.ConnectionCount() == 1);
}

TESSERA_TEST(max_inputs_per_tick_outside_1_to_64_is_refused) {
    for (const uint32_t bad : {0u, 65u}) {
        bool refused = false;
        try {
            ServerHarness h(LimitedConfig(bad));
        } catch (const std::invalid_argument& error) {
            refused = std::string(error.what()).find("max_inputs_per_tick") != std::string::npos;
        }
        TESSERA_CHECK(refused);
    }
    for (const uint32_t good : {1u, 64u}) {
        ServerHarness h(LimitedConfig(good));
        TESSERA_CHECK(h.server.ConnectionCount() == 0);
    }
}
