// SPDX-License-Identifier: Apache-2.0
#include "server_test_support.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <random>
#include <stdexcept>
#include <vector>

using namespace network_test;

TESSERA_TEST(a_valid_hello_is_accepted_and_the_connection_exists_in_both_engines) {
    ServerHarness h;
    FakeClient& client = h.ConnectAndGreet();
    client.Collect();
    const std::vector<ServerHello> hellos = client.Hellos();
    TESSERA_CHECK(hellos.size() == 1 && hellos[0].accepted && hellos[0].reason == RejectReason::None);
    TESSERA_CHECK(hellos.size() == 1 && hellos[0].connection_id == 0 &&
                  hellos[0].version == kProtocolVersion);
    TESSERA_CHECK(hellos.size() == 1 && hellos[0].tick_rate == 30 && hellos[0].max_visible == 1000);
    TESSERA_CHECK(hellos.size() == 1 && hellos[0].max_baseline_age_ticks == 32);
    TESSERA_CHECK((h.connected == std::vector<ConnectionId>{0}));
    TESSERA_CHECK(h.server.ConnectionCount() == 1);
    const Observer eye{0.0f, 0.0f, 0.0f};
    TESSERA_CHECK(
        h.server.Relevancy().SetObservers(0, std::span<const Observer>(&eye, 1))); // the connection exists
    TESSERA_CHECK(h.server.Replication().Connection(0) != nullptr);
    TESSERA_CHECK(h.server.Stats().handshakes_accepted == 1);
}

TESSERA_TEST(version_mismatch_schema_mismatch_and_a_full_server_are_rejected_and_disconnected) {
    struct Case {
        uint32_t version;
        uint64_t hash_offset;
        RejectReason expected;
    };
    for (const Case& c : {Case{kProtocolVersion + 1, 0, RejectReason::VersionMismatch},
                          Case{kProtocolVersion, 1, RejectReason::SchemaMismatch}}) {
        ServerHarness h;
        FakeClient& client = h.AddClient();
        h.Step();
        client.SendHello(c.version, h.schema.Hash() + c.hash_offset);
        h.Step();
        h.Step();
        h.Step();
        const std::vector<ServerHello> hellos = client.Hellos();
        TESSERA_CHECK(hellos.size() == 1 && !hellos[0].accepted && hellos[0].reason == c.expected);
        TESSERA_CHECK(client.disconnected);
        TESSERA_CHECK(h.server.ConnectionCount() == 0 && h.connected.empty());
        TESSERA_CHECK(h.server.Stats().handshakes_rejected == 1);
    }
    ServerHarness full(SmallServerConfig(1));
    full.ConnectAndGreet();
    FakeClient& late = full.AddClient();
    full.Step();
    late.SendHello(kProtocolVersion, full.schema.Hash());
    full.Step();
    full.Step();
    full.Step();
    const std::vector<ServerHello> hellos = late.Hellos();
    TESSERA_CHECK(hellos.size() == 1 && !hellos[0].accepted && hellos[0].reason == RejectReason::ServerFull);
    TESSERA_CHECK(late.disconnected && full.server.ConnectionCount() == 1);
}

TESSERA_TEST(messages_before_the_hello_are_ignored) {
    ServerHarness h;
    FakeClient& client = h.AddClient();
    h.Step();
    client.SendAck(5, 0);
    client.SendAck(6, 1);
    h.Step();
    h.Step();
    TESSERA_CHECK(h.server.Stats().messages_ignored == 2 && h.server.Stats().protocol_errors == 0);
    TESSERA_CHECK(h.server.ConnectionCount() == 0 && !client.disconnected);
    client.SendHello(kProtocolVersion, h.schema.Hash()); // and the hello still works afterwards
    h.Step();
    h.Step();
    h.Step();
    TESSERA_CHECK(h.server.ConnectionCount() == 1);
}

TESSERA_TEST(a_second_hello_is_a_protocol_error) {
    ServerHarness h;
    FakeClient& client = h.ConnectAndGreet();
    client.SendHello(kProtocolVersion, h.schema.Hash());
    h.Step();
    h.Step();
    h.Step();
    TESSERA_CHECK(h.server.Stats().protocol_errors == 1);
    client.Collect();
    TESSERA_CHECK(client.disconnected && h.server.ConnectionCount() == 0);
    TESSERA_CHECK((h.disconnected == std::vector<ConnectionId>{0}));
}

TESSERA_TEST(connection_ids_are_the_lowest_free_ones_and_are_reused_after_a_disconnect) {
    ServerHarness h;
    FakeClient& a = h.ConnectAndGreet();
    FakeClient& b = h.ConnectAndGreet();
    FakeClient& c = h.ConnectAndGreet();
    TESSERA_CHECK((h.connected == std::vector<ConnectionId>{0, 1, 2}));
    b.transport->Disconnect(0);
    h.Step();
    h.Step();
    h.Step();
    TESSERA_CHECK((h.disconnected == std::vector<ConnectionId>{1}) && h.server.ConnectionCount() == 2);
    FakeClient& d = h.ConnectAndGreet();
    TESSERA_CHECK((h.connected == std::vector<ConnectionId>{0, 1, 2, 1}));
    d.Collect();
    TESSERA_CHECK(d.Hellos().size() == 1 && d.Hellos()[0].connection_id == 1);
    (void)a;
    (void)c;
}

TESSERA_TEST(disconnect_removes_the_connection_from_both_engines_and_calls_the_callback) {
    ServerHarness h;
    FakeClient& client = h.ConnectAndGreet();
    const Observer eye{0.0f, 0.0f, 0.0f};
    TESSERA_CHECK(h.server.Relevancy().SetObservers(0, std::span<const Observer>(&eye, 1)));
    client.transport->Disconnect(0);
    h.Step();
    h.Step();
    h.Step();
    TESSERA_CHECK(h.server.ConnectionCount() == 0);
    TESSERA_CHECK(h.server.Replication().Connection(0) == nullptr);
    TESSERA_CHECK(
        !h.server.Relevancy().SetObservers(0, std::span<const Observer>(&eye, 1))); // no such connection
    TESSERA_CHECK((h.disconnected == std::vector<ConnectionId>{0}));
}

TESSERA_TEST(a_reused_connection_id_is_registered_again_and_starts_empty) {
    ServerHarness h;
    FakeClient& first = h.ConnectAndGreet();
    first.transport->Disconnect(0);
    h.Step();
    h.Step();
    h.Step();
    h.ConnectAndGreet();
    TESSERA_CHECK((h.connected == std::vector<ConnectionId>{0, 0}));
    TESSERA_CHECK(h.server.Replication().Connection(0) != nullptr);
    TESSERA_CHECK(h.server.Replication().Connection(0)->VisibleCount() == 0);
    TESSERA_CHECK(h.server.Relevancy().VisibleCount(0) == 0);
}

TESSERA_TEST(relevancy_config_is_overridden_by_the_server_config) {
    ServerHarness h(SmallServerConfig(8)); // the relevancy config given to the server says 2 connections
    for (int i = 0; i < 8; ++i) {
        h.ConnectAndGreet();
    }
    TESSERA_CHECK(h.server.ConnectionCount() == 8);
    const Observer eye{0.0f, 0.0f, 0.0f};
    TESSERA_CHECK(h.server.Relevancy().SetObservers(7, std::span<const Observer>(&eye, 1)));
}

TESSERA_TEST(garbage_on_the_unreliable_channel_is_dropped_and_counted_but_the_peer_stays) {
    ServerHarness h;
    FakeClient& client = h.ConnectAndGreet();
    std::mt19937 random(3);
    for (int i = 0; i < 100; ++i) {
        std::vector<uint8_t> bytes(random() % 20);
        for (uint8_t& b : bytes) {
            b = static_cast<uint8_t>(random());
        }
        client.Send(Channel::Unreliable, bytes);
    }
    h.Step();
    h.Step();
    TESSERA_CHECK(h.server.Stats().protocol_errors == 0 && h.server.ConnectionCount() == 1 &&
                  !client.disconnected);
    TESSERA_CHECK(h.server.Stats().malformed_messages + h.server.Stats().messages_ignored >= 50);
}

TESSERA_TEST(garbage_on_the_reliable_channel_breaks_the_protocol_and_the_peer_is_disconnected) {
    ServerHarness h;
    FakeClient& client = h.ConnectAndGreet();
    client.Send(Channel::ReliableOrdered, std::vector<uint8_t>{0xFF, 0x00, 0x42});
    h.Step();
    h.Step();
    h.Step();
    client.Collect();
    TESSERA_CHECK(h.server.Stats().protocol_errors == 1 && client.disconnected &&
                  h.server.ConnectionCount() == 0);
}

TESSERA_TEST(a_message_on_the_wrong_channel_is_malformed) {
    ServerHarness h;
    FakeClient& client = h.AddClient();
    h.Step();
    std::vector<uint8_t> hello;
    Encode(ClientHello{kProtocolVersion, h.schema.Hash()}, hello);
    client.Send(Channel::Unreliable, hello); // a hello must come on the reliable channel
    h.Step();
    h.Step();
    TESSERA_CHECK(h.server.ConnectionCount() == 0 && h.server.Stats().malformed_messages == 1);
    TESSERA_CHECK(!client.disconnected);
}

TESSERA_TEST(a_connection_that_never_says_hello_does_not_take_a_slot) {
    ServerHarness h(SmallServerConfig(1));
    for (int i = 0; i < 5; ++i) {
        h.AddClient();
    }
    h.Step();
    h.Step();
    TESSERA_CHECK(h.server.ConnectionCount() == 0);
    FakeClient& real = h.ConnectAndGreet(); // the one slot is still free
    real.Collect();
    TESSERA_CHECK(h.server.ConnectionCount() == 1);
}

TESSERA_TEST(the_server_refuses_a_config_that_no_client_would_accept) {
    ReplicationSchema schema = MakeTestSchema();
    SpatialGrid grid{GridConfig{64.0f, 64.0f, 64.0f}};
    RelevancyGroups groups;
    ThreadPoolJobSystem jobs(1);
    SimulatedNetwork network{NetworkConditions{}};
    auto rejects = [&](auto edit) {
        ServerConfig config = SmallServerConfig();
        edit(config);
        try {
            ReplicationServer server(config, TestRelevancyConfig(), schema, grid, groups, jobs,
                                     network.Server());
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    TESSERA_CHECK(rejects([](ServerConfig& c) { c.tick_rate = 0; }));
    TESSERA_CHECK(rejects([](ServerConfig& c) { c.tick_rate = 1001; }));
    TESSERA_CHECK(rejects([](ServerConfig& c) { c.max_visible_per_connection = 0; }));
    TESSERA_CHECK(rejects([](ServerConfig& c) { c.max_visible_per_connection = 262145; }));
    TESSERA_CHECK(rejects([](ServerConfig& c) { c.max_baseline_age_ticks = 0; }));
    TESSERA_CHECK(rejects([](ServerConfig& c) { c.max_baseline_age_ticks = 256; }));
    TESSERA_CHECK(rejects([](ServerConfig& c) { c.max_connections = 0; }));
    TESSERA_CHECK(!rejects([](ServerConfig&) {}));
}

TESSERA_TEST(a_message_kind_a_client_never_sends_is_a_protocol_error) {
    ServerHarness h;
    FakeClient& client = h.ConnectAndGreet();
    std::vector<uint8_t> batch;
    Encode(EventBatch{1, 1, {}}, batch);
    client.Send(Channel::ReliableOrdered, batch); // only the server sends event batches
    h.Step();
    h.Step();
    h.Step();
    client.Collect();
    TESSERA_CHECK(h.server.Stats().protocol_errors == 1 && client.disconnected &&
                  h.server.ConnectionCount() == 0);
}
