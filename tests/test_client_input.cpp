// SPDX-License-Identifier: Apache-2.0
// The client's side of player input: the sequence numbers, the repeats, and what reaches the server when the
// network loses messages.
#include "server_test_support.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <random>
#include <span>
#include <vector>

using namespace network_test;

namespace {

// Brings a harness's client to `Connected`.
void Connect(ClientHarness& h) {
    h.Step();
    h.Accept();
    h.Step();
}

uint32_t Send(ClientReplica& client, uint8_t byte) {
    const uint8_t bytes[] = {byte};
    return client.SendInput(bytes);
}

// How many `Input` messages from the client travelled on the given channel.
size_t InputMessagesOn(const ClientHarness& h, Channel channel) {
    size_t count = 0;
    for (const TransportEvent& event : h.from_client) {
        Input input;
        if (event.kind == TransportEvent::Kind::Message && event.channel == channel &&
            Decode(event.data, &input)) {
            ++count;
        }
    }
    return count;
}

// A transport that passes everything on, except that it drops some of the unreliable `Input` messages, and
// remembers which, in the order they were sent.
class DroppingTransport final : public ITransport {
public:
    DroppingTransport(ITransport& inner, double chance) : inner_(inner), chance_(chance) {}

    void Poll(std::vector<TransportEvent>& out) override {
        inner_.Poll(out);
    }
    bool Send(TransportConnection connection, Channel channel, std::span<const uint8_t> data) override {
        MessageKind kind{};
        if (channel == Channel::Unreliable && PeekKind(data, &kind) && kind == MessageKind::Input) {
            const bool drop = std::uniform_real_distribution<double>(0.0, 1.0)(random_) < chance_;
            dropped.push_back(drop);
            if (drop) {
                return true; // as far as the sender can tell, it went
            }
        }
        return inner_.Send(connection, channel, data);
    }
    void Disconnect(TransportConnection connection) override {
        inner_.Disconnect(connection);
    }
    TransportStats Stats() const noexcept override {
        return inner_.Stats();
    }

    std::vector<bool> dropped; // for each `Input` message sent, in order

private:
    ITransport& inner_;
    double chance_;
    std::mt19937 random_{99};
};

} // namespace

TESSERA_TEST(send_input_before_the_connection_is_accepted_returns_0_and_sends_nothing) {
    ClientHarness h;
    TESSERA_CHECK(Send(h.client, 1) == 0); // Connecting
    h.Step();
    TESSERA_CHECK(h.client.State() == ClientState::Handshaking);
    TESSERA_CHECK(Send(h.client, 1) == 0);
    h.Step();
    TESSERA_CHECK(h.MessagesFromClient() == 1); // only the hello
    TESSERA_CHECK(h.Inputs().empty() && h.client.Stats().inputs_refused == 0 &&
                  h.client.Stats().inputs_sent == 0);
}

TESSERA_TEST(send_input_returns_1_2_3_and_sends_one_unreliable_message_each) {
    ClientHarness h;
    Connect(h);
    TESSERA_CHECK(h.client.State() == ClientState::Connected);
    TESSERA_CHECK(Send(h.client, 10) == 1);
    TESSERA_CHECK(Send(h.client, 11) == 2);
    TESSERA_CHECK(Send(h.client, 12) == 3);
    h.Step();
    TESSERA_CHECK(h.Inputs().size() == 3);
    TESSERA_CHECK(InputMessagesOn(h, Channel::Unreliable) == 3 &&
                  InputMessagesOn(h, Channel::ReliableOrdered) == 0);
    TESSERA_CHECK(h.client.Stats().inputs_sent == 3);
}

TESSERA_TEST(each_message_carries_the_current_input_and_the_three_before) {
    ClientHarness h;
    Connect(h);
    for (uint8_t k = 1; k <= 6; ++k) {
        TESSERA_CHECK(Send(h.client, k) == k);
    }
    h.Step();
    const std::vector<Input> messages = h.Inputs();
    TESSERA_CHECK(messages.size() == 6);
    for (uint32_t k = 1; k <= 6 && k <= messages.size(); ++k) {
        const Input& message = messages[k - 1];
        const uint32_t first = k > 3 ? k - 3 : 1;
        TESSERA_CHECK(message.entries.size() == k - first + 1);
        for (uint32_t i = 0; i < message.entries.size(); ++i) {
            TESSERA_CHECK(message.entries[i].sequence == first + i);
            TESSERA_CHECK(message.entries[i].size == 1 && message.entries[i].bytes[0] == first + i);
        }
    }
}

TESSERA_TEST(an_input_is_repeated_in_three_more_messages_and_then_stops) {
    ClientHarness h;
    Connect(h);
    for (uint8_t k = 1; k <= 6; ++k) {
        Send(h.client, k);
    }
    h.Step();
    const std::vector<Input> messages = h.Inputs();
    TESSERA_CHECK(messages.size() == 6);
    for (size_t m = 0; m < messages.size(); ++m) {
        bool has_first = false;
        for (const InputEntry& entry : messages[m].entries) {
            has_first = has_first || entry.sequence == 1;
        }
        TESSERA_CHECK(has_first == (m < 4)); // in messages 1 to 4, not in the fifth
    }
}

TESSERA_TEST(send_input_with_too_many_bytes_returns_0_and_counts_it) {
    ClientHarness h;
    Connect(h);
    const std::vector<uint8_t> too_many(kMaxInputBytes + 1, 0x77);
    TESSERA_CHECK(h.client.SendInput(too_many) == 0);
    h.Step();
    TESSERA_CHECK(h.client.Stats().inputs_refused == 1 && h.Inputs().empty());
    const std::vector<uint8_t> largest(kMaxInputBytes, 0x77);
    TESSERA_CHECK(h.client.SendInput(largest) == 1); // the refused one took no sequence number
    TESSERA_CHECK(h.client.SendInput(std::span<const uint8_t>()) == 2);
    h.Step();
    TESSERA_CHECK(h.Inputs().size() == 2 && h.client.Stats().inputs_refused == 1);
}

TESSERA_TEST(send_input_after_disconnect_rejection_or_protocol_error_returns_0) {
    {
        ClientHarness h;
        Connect(h);
        TESSERA_CHECK(Send(h.client, 1) == 1);
        h.client.Disconnect();
        TESSERA_CHECK(Send(h.client, 2) == 0);
    }
    {
        ClientHarness h;
        h.Step();
        h.Reject(RejectReason::ServerFull);
        h.Step();
        TESSERA_CHECK(h.client.State() == ClientState::Rejected);
        TESSERA_CHECK(Send(h.client, 1) == 0);
    }
    {
        ClientHarness h;
        Connect(h);
        h.SendBatch(5, 1, {}); // the first batch has to be number 1: the server is broken
        h.Step();
        TESSERA_CHECK(h.client.State() == ClientState::Disconnected);
        TESSERA_CHECK(Send(h.client, 1) == 0);
    }
}

TESSERA_TEST(the_client_ignores_an_input_message_from_the_server) {
    Input input;
    InputEntry entry;
    entry.sequence = 4;
    entry.size = 1;
    input.entries.push_back(entry);
    std::vector<uint8_t> bytes;
    Encode(input, bytes);
    {
        ClientHarness h;
        Connect(h);
        h.Send(Channel::Unreliable, bytes);
        h.Step();
        TESSERA_CHECK(h.client.Stats().malformed_messages == 1);
        TESSERA_CHECK(h.client.State() == ClientState::Connected);
    }
    {
        ClientHarness h;
        Connect(h);
        h.Send(Channel::ReliableOrdered, bytes);
        h.Step();
        TESSERA_CHECK(h.client.Stats().protocol_errors == 1);
        TESSERA_CHECK(h.client.State() == ClientState::Disconnected);
    }
}

TESSERA_TEST(two_clients_in_one_process_have_their_own_sequences) {
    ClientHarness a;
    ClientHarness b;
    Connect(a);
    Connect(b);
    TESSERA_CHECK(Send(a.client, 1) == 1);
    TESSERA_CHECK(Send(b.client, 1) == 1);
    TESSERA_CHECK(Send(a.client, 2) == 2);
    TESSERA_CHECK(Send(a.client, 3) == 3);
    TESSERA_CHECK(Send(b.client, 2) == 2);
    TESSERA_CHECK(Send(b.client, 3) == 3);
}

TESSERA_TEST(input_delivery_matches_the_loss_oracle_at_30_percent_loss) {
    constexpr uint32_t kSent = 600;
    ServerHarness h(SmallServerConfig());
    DroppingTransport dropping(*h.network.AddClient(), 0.3);
    ClientReplica client(dropping, h.schema);
    for (int i = 0; i < 20 && client.State() != ClientState::Connected; ++i) {
        h.Step();
        client.Poll();
    }
    TESSERA_CHECK(client.State() == ClientState::Connected);
    for (uint32_t i = 1; i <= kSent; ++i) {
        TESSERA_CHECK(Send(client, static_cast<uint8_t>(i)) == i);
        h.Step();
        client.Poll();
    }
    // Input s is delivered if, and only if, one of the messages s to s + 3 got through: each carries it.
    TESSERA_CHECK(dropping.dropped.size() == kSent);
    std::vector<uint32_t> expected;
    for (uint32_t s = 1; s <= kSent && dropping.dropped.size() == kSent; ++s) {
        bool arrived = false;
        for (uint32_t m = s; m <= s + 3 && m <= kSent; ++m) {
            arrived = arrived || !dropping.dropped[m - 1];
        }
        if (arrived) {
            expected.push_back(s);
        }
    }
    std::vector<uint32_t> delivered;
    for (const ServerHarness::DeliveredInput& input : h.inputs) {
        delivered.push_back(input.sequence);
    }
    TESSERA_CHECK(delivered == expected);
    TESSERA_CHECK(h.server.Stats().inputs_delivered == expected.size() && h.server.Stats().inputs_old == 0);
    TESSERA_CHECK(h.server.Stats().inputs_repeated > 0);
    size_t lost = 0;
    for (const bool drop : dropping.dropped) {
        lost += drop ? 1 : 0;
    }
    TESSERA_CHECK(lost > 100); // the loss really happened
    std::printf("  loss 30%%: %zu of %u messages dropped, %zu of %u inputs delivered\n", lost, kSent,
                expected.size(), kSent);
}

TESSERA_TEST(input_delivery_survives_the_simulated_network_loss_delay_and_duplicates) {
    constexpr uint32_t kSent = 600;
    NetworkConditions conditions;
    conditions.loss = 0.3;
    conditions.duplicate = 0.1;
    conditions.min_delay_ticks = 0;
    conditions.max_delay_ticks = 2;
    conditions.seed = 5;
    ServerHarness h(SmallServerConfig(), conditions);
    ITransport* transport = h.network.AddClient();
    ClientReplica client(*transport, h.schema);
    for (int i = 0; i < 40 && client.State() != ClientState::Connected; ++i) {
        h.Step();
        client.Poll();
    }
    TESSERA_CHECK(client.State() == ClientState::Connected);
    for (uint32_t i = 1; i <= kSent; ++i) {
        Send(client, static_cast<uint8_t>(i));
        h.Step();
        client.Poll();
    }
    for (int i = 0; i < 20; ++i) {
        h.Step();
        client.Poll();
    }
    bool rising = true;
    uint32_t previous = 0;
    for (const ServerHarness::DeliveredInput& input : h.inputs) {
        rising = rising && input.sequence > previous; // never twice, never backwards
        previous = input.sequence;
    }
    TESSERA_CHECK(rising);
    TESSERA_CHECK(h.inputs.size() >= 570);
    TESSERA_CHECK(transport->Stats().messages_lost > 100);
    TESSERA_CHECK(h.server.Stats().inputs_repeated > 0);
    std::printf("  loss 30%% with delay and duplicates: %llu messages lost, %zu of %u inputs delivered\n",
                static_cast<unsigned long long>(transport->Stats().messages_lost), h.inputs.size(), kSent);
}
