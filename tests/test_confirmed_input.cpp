// SPDX-License-Identifier: Apache-2.0
// The input sequence that each state buffer confirms to the player: the newest input of that connection that
// the state in the buffer already reflects.
#include "server_test_support.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <map>
#include <vector>

using namespace network_test;

namespace {

// A mob in view of the player that moves a little on every call, so that there is something to say in each
// tick.
struct Walker {
    ServerHarness& h;
    EntityId mob;
    float x = 1.0f;

    explicit Walker(ServerHarness& harness) : h(harness), mob(harness.AddMob(1.0f, 1.0f)) {
        // Every player that joins looks at the middle of the world, where the mob is.
        h.on_connect_hook = [this](ConnectionId id) { h.Observe(id, 0.0f, 0.0f); };
    }
    void Move() {
        x += 0.5f;
        h.MoveMob(mob, x, 1.0f);
    }
};

// One tick with a move, so that every buffer has a record.
void StepMoving(ServerHarness& h, Walker& walker) {
    walker.Move();
    h.Step();
}

// Lets the last buffers arrive.
void Drain(ServerHarness& h, FakeClient& client) {
    h.Step();
    client.Collect();
}

std::vector<uint32_t> Confirmed(const FakeClient& client) {
    std::vector<uint32_t> found;
    for (const FakeClient::Buffer& buffer : client.Buffers()) {
        found.push_back(buffer.header.last_input_sequence);
    }
    return found;
}

// A client that sees the mob of the `Walker`.
FakeClient& JoinNearTheMob(ServerHarness& h) {
    return h.ConnectAndGreet();
}

} // namespace

TESSERA_TEST(the_state_buffer_carries_0_until_an_input_is_delivered) {
    ServerHarness h;
    Walker walker(h);
    FakeClient& client = JoinNearTheMob(h);
    for (int i = 0; i < 5; ++i) {
        StepMoving(h, walker);
    }
    Drain(h, client);
    const std::vector<uint32_t> before = Confirmed(client);
    TESSERA_CHECK(before.size() >= 4);
    for (const uint32_t sequence : before) {
        TESSERA_CHECK(sequence == 0);
    }
    client.SendInput(Channel::Unreliable, {1});
    StepMoving(h, walker); // reads the input; the state of this tick was made before it
    StepMoving(h, walker); // the state of this tick reflects it
    Drain(h, client);
    const std::vector<uint32_t> after = Confirmed(client);
    TESSERA_CHECK(after.size() > before.size() + 1);
    TESSERA_CHECK(after.back() == 1);
}

TESSERA_TEST(a_buffer_carries_the_inputs_delivered_before_its_update_not_in_it) {
    ServerHarness h;
    Walker walker(h);
    FakeClient& client = JoinNearTheMob(h);
    StepMoving(h, walker);
    // The input numbered k is sent before the tick `sent_at[k]`, so the server reads it in that tick's update.
    std::map<uint32_t, uint64_t> sent_at;
    for (uint32_t k = 1; k <= 12; ++k) {
        client.SendInput(Channel::Unreliable, {k});
        sent_at[k] = h.tick;
        StepMoving(h, walker);
    }
    StepMoving(h, walker); // the state of this tick reflects the last input
    Drain(h, client);
    size_t checked = 0;
    for (const FakeClient::Buffer& buffer : client.Buffers()) {
        // The state of tick t was simulated before the update of tick t read its inputs: it reflects the
        // inputs read by the updates up to tick t - 1.
        uint32_t expected = 0;
        for (const auto& [sequence, tick] : sent_at) {
            if (tick + 1 <= buffer.header.tick) {
                expected = sequence;
            }
        }
        TESSERA_CHECK(buffer.header.last_input_sequence == expected);
        ++checked;
    }
    TESSERA_CHECK(checked >= 12);
    // And the last buffers do reach the newest input.
    TESSERA_CHECK(Confirmed(client).back() == 12);
}

TESSERA_TEST(the_confirmed_sequence_never_goes_down) {
    ServerHarness h;
    Walker walker(h);
    FakeClient& client = JoinNearTheMob(h);
    StepMoving(h, walker);
    const std::vector<std::vector<uint32_t>> messages = {{5}, {3}, {5}, {9}, {7, 8}, {9}, {10, 11}, {2}, {11}};
    for (const std::vector<uint32_t>& message : messages) {
        if (message.size() == 1) {
            client.SendInput(Channel::Unreliable, {message[0]});
        } else {
            client.SendInput(Channel::Unreliable, {message[0], message[1]});
        }
        StepMoving(h, walker);
    }
    Drain(h, client);
    const std::vector<uint32_t> values = Confirmed(client);
    TESSERA_CHECK(values.size() >= messages.size());
    for (size_t i = 1; i < values.size(); ++i) {
        TESSERA_CHECK(values[i] >= values[i - 1]);
    }
    TESSERA_CHECK(values.back() == 11);
}

TESSERA_TEST(a_dropped_input_is_not_confirmed) {
    ServerConfig config = SmallServerConfig();
    config.max_inputs_per_tick = 3;
    ServerHarness h(config);
    Walker walker(h);
    FakeClient& client = JoinNearTheMob(h);
    StepMoving(h, walker);
    client.SendInput(Channel::Unreliable, {1, 2, 3, 4}); // the 4 goes over the limit and is dropped
    StepMoving(h, walker);
    StepMoving(h, walker);
    StepMoving(h, walker);
    Drain(h, client);
    TESSERA_CHECK(h.server.Stats().inputs_dropped == 1);
    TESSERA_CHECK(Confirmed(client).back() == 3);
    client.SendInput(Channel::Unreliable, {2, 3, 4}); // the repeat delivers it
    StepMoving(h, walker);
    StepMoving(h, walker);
    Drain(h, client);
    TESSERA_CHECK(Confirmed(client).back() == 4);
}

TESSERA_TEST(a_late_or_repeated_input_does_not_move_the_confirmed_sequence) {
    ServerHarness h;
    Walker walker(h);
    FakeClient& client = JoinNearTheMob(h);
    StepMoving(h, walker);
    client.SendInput(Channel::Unreliable, {20});
    StepMoving(h, walker);
    StepMoving(h, walker);
    client.SendInput(Channel::Unreliable, {20}); // repeated
    StepMoving(h, walker);
    client.SendInput(Channel::Unreliable, {4}); // late
    StepMoving(h, walker);
    StepMoving(h, walker);
    Drain(h, client);
    const std::vector<uint32_t> values = Confirmed(client);
    TESSERA_CHECK(!values.empty() && values.back() == 20);
    TESSERA_CHECK(h.server.Stats().inputs_repeated == 1 && h.server.Stats().inputs_old == 1);
    for (const uint32_t value : values) {
        TESSERA_CHECK(value == 0 || value == 20);
    }
}

TESSERA_TEST(the_confirmed_sequence_is_per_connection) {
    ServerHarness h;
    Walker walker(h);
    FakeClient& a = JoinNearTheMob(h);
    FakeClient& b = JoinNearTheMob(h);
    StepMoving(h, walker);
    a.SendInput(Channel::Unreliable, {30});
    b.SendInput(Channel::Unreliable, {7});
    StepMoving(h, walker);
    StepMoving(h, walker);
    StepMoving(h, walker);
    h.Step();
    a.Collect();
    b.Collect();
    const std::vector<uint32_t> a_values = Confirmed(a);
    const std::vector<uint32_t> b_values = Confirmed(b);
    TESSERA_CHECK(!a_values.empty() && a_values.back() == 30);
    TESSERA_CHECK(!b_values.empty() && b_values.back() == 7);
}

TESSERA_TEST(a_player_in_a_reused_slot_starts_confirming_from_0) {
    ServerHarness h;
    Walker walker(h);
    FakeClient& first = JoinNearTheMob(h);
    first.SendInput(Channel::Unreliable, {100});
    StepMoving(h, walker);
    StepMoving(h, walker);
    StepMoving(h, walker);
    first.Collect();
    TESSERA_CHECK(!Confirmed(first).empty() && Confirmed(first).back() == 100);
    first.transport->Disconnect(0);
    StepMoving(h, walker);
    StepMoving(h, walker);
    TESSERA_CHECK((h.disconnected == std::vector<ConnectionId>{0}));
    FakeClient& second = h.ConnectAndGreet();
    TESSERA_CHECK((h.connected == std::vector<ConnectionId>{0, 0}));
    for (int i = 0; i < 4; ++i) {
        StepMoving(h, walker);
    }
    Drain(h, second);
    const std::vector<uint32_t> values = Confirmed(second);
    TESSERA_CHECK(!values.empty());
    for (const uint32_t value : values) {
        TESSERA_CHECK(value == 0);
    }
    second.SendInput(Channel::Unreliable, {1});
    StepMoving(h, walker);
    StepMoving(h, walker);
    Drain(h, second);
    TESSERA_CHECK(Confirmed(second).back() == 1);
}

TESSERA_TEST(the_confirmed_sequence_follows_a_wrap_around_of_uint32) {
    ServerHarness h;
    Walker walker(h);
    FakeClient& client = JoinNearTheMob(h);
    StepMoving(h, walker);
    for (const uint32_t sequence : {0xFFFFFFFEu, 0xFFFFFFFFu, 1u, 2u}) {
        client.SendInput(Channel::Unreliable, {sequence});
        StepMoving(h, walker);
    }
    StepMoving(h, walker);
    Drain(h, client);
    std::vector<uint32_t> distinct;
    for (const uint32_t value : Confirmed(client)) {
        if (distinct.empty() || distinct.back() != value) {
            distinct.push_back(value);
        }
    }
    TESSERA_CHECK((distinct == std::vector<uint32_t>{0, 0xFFFFFFFEu, 0xFFFFFFFFu, 1, 2}));
}

TESSERA_TEST(a_player_whose_state_does_not_change_still_gets_a_header_only_buffer_when_the_sequence_moves) {
    ServerHarness h;
    FakeClient& client = h.ConnectAndGreet(); // no entity at all: nothing to say about the world
    const uint64_t sent_before = h.server.Stats().state_buffers_sent;
    for (uint32_t k = 1; k <= 6; ++k) {
        client.SendInput(Channel::Unreliable, {k});
        h.Step();
    }
    h.Step();
    h.Step();
    client.Collect();
    const std::vector<FakeClient::Buffer> buffers = client.Buffers();
    TESSERA_CHECK(buffers.size() == 6);
    uint32_t previous = 0;
    for (const FakeClient::Buffer& buffer : buffers) {
        TESSERA_CHECK(buffer.payload.size() == 1); // just the end of the records
        TESSERA_CHECK(buffer.header.last_input_sequence > previous);
        previous = buffer.header.last_input_sequence;
    }
    TESSERA_CHECK(previous == 6);
    TESSERA_CHECK(h.server.Stats().state_buffers_sent - sent_before == 6);
    TESSERA_CHECK(h.server.Stats().input_confirmations_sent == 6);
    // One buffer for each step forward and none without one.
    const uint64_t total = h.server.Stats().state_buffers_sent;
    h.Step();
    h.Step();
    h.Step();
    TESSERA_CHECK(h.server.Stats().state_buffers_sent == total);
}

TESSERA_TEST(an_idle_player_that_sends_no_input_still_costs_no_buffer) {
    ServerHarness h;
    h.ConnectAndGreet();
    const uint64_t total = h.server.Stats().state_buffers_sent;
    for (int i = 0; i < 10; ++i) {
        h.Step();
    }
    TESSERA_CHECK(h.server.Stats().state_buffers_sent == total);
    TESSERA_CHECK(h.server.Stats().input_confirmations_sent == 0);
}

TESSERA_TEST(a_header_only_buffer_is_confirmed_by_the_client_without_harm) {
    ServerHarness h;
    ClientReplica client(*h.network.AddClient(), h.schema);
    for (int i = 0; i < 4; ++i) {
        h.Step();
        client.Poll();
    }
    TESSERA_CHECK(client.State() == ClientState::Connected);
    for (int i = 0; i < 8; ++i) {
        client.SendInput(std::vector<uint8_t>{1, 2});
        h.Step();
        client.Poll();
    }
    h.Step();
    client.Poll();
    TESSERA_CHECK(client.Stats().buffers_applied >= 7);
    TESSERA_CHECK(client.Stats().buffers_refused == 0 && client.Stats().protocol_errors == 0);
    TESSERA_CHECK(client.Stats().acks_sent >= 1);
    TESSERA_CHECK(h.server.Stats().protocol_errors == 0 && h.server.Stats().messages_ignored == 0);
    TESSERA_CHECK(h.server.Stats().input_confirmations_sent >= 7);
}
