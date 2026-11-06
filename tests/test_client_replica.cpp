// SPDX-License-Identifier: Apache-2.0
#include "network_test_support.hpp"
#include "test_support.hpp"

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
