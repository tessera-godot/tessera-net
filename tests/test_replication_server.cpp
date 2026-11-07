// SPDX-License-Identifier: Apache-2.0
#include "server_test_support.hpp"
#include "tessera/net/replication/record_codec.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
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

// ---- The network phase of a tick ---------------------------------------------------------------------

namespace {

// The position of a record read from the head of a buffer: the baseline age of its first record (0 if
// complete).
uint32_t FirstRecordAge(const std::vector<uint8_t>& payload, bool* complete) {
    BitReader reader(payload);
    uint32_t gap = 0, age = 0;
    bool is_complete = false;
    if (!ReadRecordHead(reader, &gap, &is_complete, &age)) {
        *complete = false;
        return 0;
    }
    *complete = is_complete;
    return age;
}

size_t CountBuffers(const FakeClient& client) {
    return client.Buffers().size();
}

} // namespace

TESSERA_TEST(events_are_sent_as_numbered_batches_only_when_there_are_events) {
    ServerHarness h;
    h.on_connect_hook = [&](ConnectionId id) { h.Observe(id, 0.0f, 0.0f); };
    const EntityId near = h.AddMob(10.0f, 10.0f);
    FakeClient& client = h.ConnectAndGreet();
    for (int i = 0; i < 4; ++i) {
        h.Step(); // nothing changes in what the player sees
    }
    client.Collect();
    std::vector<EventBatch> batches = client.Batches();
    TESSERA_CHECK(batches.size() == 1 && batches[0].number == 1);
    TESSERA_CHECK(batches.size() == 1 && batches[0].events.size() == 1 && batches[0].events[0].id == near);
    const EntityId second = h.AddMob(20.0f, 20.0f);
    h.Step();
    h.Step();
    client.Collect();
    batches = client.Batches();
    TESSERA_CHECK(batches.size() == 2 && batches[1].number == 2);
    TESSERA_CHECK(batches.size() == 2 && batches[1].events.size() == 1 && batches[1].events[0].id == second);
    TESSERA_CHECK(h.server.Stats().event_batches_sent == 2);
}

TESSERA_TEST(the_event_batch_is_sent_before_the_state_buffer_that_cites_it) {
    ServerHarness h;
    h.on_connect_hook = [&](ConnectionId id) { h.Observe(id, 0.0f, 0.0f); };
    h.AddMob(10.0f, 10.0f);
    FakeClient& client = h.ConnectAndGreet();
    client.Collect();
    int batch_at = -1, buffer_at = -1;
    for (size_t i = 0; i < client.inbox.size(); ++i) {
        EventBatch batch;
        StateBufferHeader header{};
        std::span<const uint8_t> payload;
        if (client.inbox[i].kind != TransportEvent::Kind::Message) {
            continue;
        }
        if (Decode(client.inbox[i].data, &batch) && batch_at < 0) {
            batch_at = static_cast<int>(i);
        }
        if (DecodeStateBuffer(client.inbox[i].data, &header, &payload) && buffer_at < 0) {
            buffer_at = static_cast<int>(i);
            TESSERA_CHECK(header.batches_sent == 1);
        }
    }
    TESSERA_CHECK(batch_at >= 0 && buffer_at >= 0 && batch_at < buffer_at);
}

TESSERA_TEST(state_buffers_are_sent_only_when_they_have_records) {
    ServerHarness h;
    h.on_connect_hook = [&](ConnectionId id) { h.Observe(id, 0.0f, 0.0f); };
    h.AddMob(10.0f, 10.0f);
    FakeClient& client = h.ConnectAndGreet();
    client.Collect();
    TESSERA_CHECK(CountBuffers(client) >= 1);
    client.SendAck(h.tick - 1, 0); // the player confirms everything it has, so the entity settles
    h.Step();
    h.Step();
    const size_t before = h.server.Stats().state_buffers_sent;
    for (int i = 0; i < 5; ++i) {
        h.Step(); // an idle player costs nothing
    }
    TESSERA_CHECK(h.server.Stats().state_buffers_sent == before);
}

TESSERA_TEST(an_ack_settles_the_entity_and_the_server_stops_sending_it) {
    ServerHarness h;
    h.on_connect_hook = [&](ConnectionId id) { h.Observe(id, 0.0f, 0.0f); };
    const EntityId mob = h.AddMob(10.0f, 10.0f);
    FakeClient& client = h.ConnectAndGreet();
    for (int i = 0; i < 3; ++i) {
        h.Step(); // unconfirmed: it is sent again each tick
    }
    const size_t unconfirmed = h.server.Stats().state_buffers_sent;
    TESSERA_CHECK(unconfirmed >= 3);
    TESSERA_CHECK(h.server.Replication().Connection(0)->IsPending(0));
    client.SendAck(h.tick - 1, 0);
    h.Step();
    h.Step();
    TESSERA_CHECK(!h.server.Replication().Connection(0)->IsPending(0));
    const size_t settled = h.server.Stats().state_buffers_sent;
    for (int i = 0; i < 4; ++i) {
        h.Step();
    }
    TESSERA_CHECK(h.server.Stats().state_buffers_sent == settled);
    (void)mob;
}

TESSERA_TEST(a_later_ack_with_a_mask_confirms_the_ticks_before_it_and_the_baseline_age_shows_it) {
    ServerHarness h;
    h.on_connect_hook = [&](ConnectionId id) { h.Observe(id, 0.0f, 0.0f); };
    const EntityId mob = h.AddMob(10.0f, 10.0f);
    FakeClient& client = h.ConnectAndGreet();
    for (int i = 0; i < 3; ++i) {
        h.MoveMob(mob, 10.0f + static_cast<float>(i + 1), 10.0f);
        h.Step(); // a different state on every tick
    }
    const uint64_t last_sent = h.tick - 1;
    client.SendAck(last_sent, 0b11); // the newest tick and the two before it
    h.Step();
    h.Step();
    h.MoveMob(mob, 40.0f, 10.0f); // changes after being settled
    h.Step();
    h.Step();
    client.Collect();
    const std::vector<FakeClient::Buffer> buffers = client.Buffers();
    TESSERA_CHECK(!buffers.empty());
    bool complete = true;
    const uint32_t age = FirstRecordAge(buffers.back().payload, &complete);
    TESSERA_CHECK(!complete); // against a confirmed state, not in full
    TESSERA_CHECK(age >= 1 && age <= 4);
}

TESSERA_TEST(an_old_or_repeated_ack_changes_nothing_and_an_ack_for_an_unsent_tick_is_harmless) {
    ServerHarness h;
    h.on_connect_hook = [&](ConnectionId id) { h.Observe(id, 0.0f, 0.0f); };
    h.AddMob(10.0f, 10.0f);
    FakeClient& client = h.ConnectAndGreet();
    client.SendAck(h.tick - 1, 0);
    h.Step();
    h.Step();
    TESSERA_CHECK(!h.server.Replication().Connection(0)->IsPending(0));
    client.SendAck(h.tick - 1, 0);          // the same again
    client.SendAck(1, 0xFFFFFFFFu);         // very old
    client.SendAck(1'000'000, 0xFFFFFFFFu); // from the future
    h.Step();
    h.Step();
    TESSERA_CHECK(h.server.Stats().protocol_errors == 0 && h.server.ConnectionCount() == 1);
    TESSERA_CHECK(!h.server.Replication().Connection(0)->IsPending(0));
}

TESSERA_TEST(acks_from_one_connection_do_not_confirm_another) {
    ServerHarness h;
    h.on_connect_hook = [&](ConnectionId id) { h.Observe(id, 0.0f, 0.0f); };
    h.AddMob(10.0f, 10.0f);
    FakeClient& a = h.ConnectAndGreet();
    FakeClient& b = h.ConnectAndGreet();
    h.Step();
    h.Step();
    a.SendAck(h.tick - 1, 0xFFFFFFFFu);
    h.Step();
    h.Step();
    TESSERA_CHECK(!h.server.Replication().Connection(0)->IsPending(0)); // a is confirmed
    TESSERA_CHECK(h.server.Replication().Connection(1)->IsPending(0));  // b is not
    (void)b;
}

TESSERA_TEST(observers_set_in_the_connected_callback_take_effect_in_the_same_update) {
    ServerHarness h;
    h.on_connect_hook = [&](ConnectionId id) { h.Observe(id, 0.0f, 0.0f); };
    h.AddMob(10.0f, 10.0f);
    FakeClient& client = h.AddClient();
    h.Step();
    client.SendHello(kProtocolVersion, h.schema.Hash());
    h.Step(); // the hello is read, the player is accepted, the callback sets the observer, the phase runs
    TESSERA_CHECK(h.server.Stats().event_batches_sent == 1 && h.server.Stats().state_buffers_sent == 1);
}

TESSERA_TEST(a_disconnect_in_the_middle_of_a_tick_releases_the_connection_before_the_phase_runs) {
    ServerHarness h;
    h.on_connect_hook = [&](ConnectionId id) { h.Observe(id, 0.0f, 0.0f); };
    h.AddMob(10.0f, 10.0f);
    FakeClient& a = h.ConnectAndGreet();
    FakeClient& b = h.ConnectAndGreet();
    h.Step();
    a.transport->Disconnect(0);
    const size_t before = h.server.Stats().state_buffers_sent;
    h.Step();
    h.Step();
    b.Collect();
    TESSERA_CHECK(h.server.ConnectionCount() == 1 && h.server.Replication().Connection(0) == nullptr);
    TESSERA_CHECK(h.server.Stats().state_buffers_sent - before <= 2); // only b is still being sent to
    TESSERA_CHECK(CountBuffers(b) >= 1);
}

TESSERA_TEST(refresh_forwards_to_the_replication_engine) {
    ServerHarness h;
    h.AddMob(1.0f, 1.0f);
    h.AddMob(2.0f, 2.0f);
    TESSERA_CHECK(h.server.Refresh(h.table) == 2); // both were flagged when they were created
    TESSERA_CHECK(h.server.Refresh(h.table) == 0);
    TESSERA_CHECK(h.server.Replication().Store().Changed().size() == 2);
}

TESSERA_TEST(the_server_tick_gives_the_same_bytes_with_1_and_8_workers) {
    auto run = [](uint32_t workers) {
        ServerHarness h(SmallServerConfig(4), NetworkConditions{}, workers);
        h.on_connect_hook = [&](ConnectionId id) { h.Observe(id, 20.0f * static_cast<float>(id), 0.0f); };
        std::vector<EntityId> mobs;
        for (int i = 0; i < 40; ++i) {
            mobs.push_back(h.AddMob(static_cast<float>(i * 7 % 90), static_cast<float>(i * 13 % 90)));
        }
        std::vector<FakeClient*> clients;
        for (int i = 0; i < 3; ++i) {
            clients.push_back(&h.ConnectAndGreet());
        }
        for (int step = 0; step < 30; ++step) {
            for (size_t i = 0; i < mobs.size(); i += 3) {
                h.MoveMob(mobs[i], static_cast<float>((step * 3 + i * 7) % 100),
                          static_cast<float>((step + i * 5) % 100));
            }
            h.Step();
        }
        h.Step();
        uint64_t hash = 0xCBF29CE484222325ull;
        for (FakeClient* client : clients) {
            client->Collect();
            for (const TransportEvent& event : client->inbox) {
                for (const uint8_t byte : event.data) {
                    hash = (hash ^ byte) * 0x100000001B3ull;
                }
            }
        }
        return hash;
    };
    TESSERA_CHECK(run(1) == run(8));
}

TESSERA_TEST(an_ack_for_a_tick_that_was_never_sent_is_dropped_and_does_not_stop_later_confirmations) {
    ServerHarness h;
    h.on_connect_hook = [&](ConnectionId id) { h.Observe(id, 0.0f, 0.0f); };
    const EntityId mob = h.AddMob(10.0f, 10.0f);
    FakeClient& client = h.ConnectAndGreet();
    h.Step();
    const uint64_t ignored_before = h.server.Stats().messages_ignored;
    client.SendAck(1'000'000, 0xFFFFFFFFu); // a tick far ahead of anything the server has sent
    h.Step();
    h.Step();
    TESSERA_CHECK(h.server.Stats().messages_ignored == ignored_before + 1); // dropped and counted
    h.MoveMob(mob, 20.0f, 10.0f); // the entity changes after the forged ack
    h.Step();
    client.SendAck(h.tick - 1, 0); // and the real player confirms it
    h.Step();
    h.Step();
    TESSERA_CHECK(!h.server.Replication().Connection(0)->IsPending(0)); // still settles
    const uint64_t settled = h.server.Stats().state_buffers_sent;
    for (int i = 0; i < 4; ++i) {
        h.Step();
    }
    TESSERA_CHECK(h.server.Stats().state_buffers_sent == settled);
    TESSERA_CHECK(h.server.Stats().protocol_errors == 0 && h.server.ConnectionCount() == 1);
}

TESSERA_TEST(a_type_that_players_can_see_but_that_has_no_replication_schema_is_refused_at_construction) {
    ReplicationSchema schema = MakeTestSchema();
    SpatialGrid grid{GridConfig{64.0f, 64.0f, 64.0f}};
    RelevancyGroups groups;
    ThreadPoolJobSystem jobs(1);
    SimulatedNetwork network{NetworkConditions{}};
    RelevancyConfig relevancy = TestRelevancyConfig();
    relevancy.types[kUnknownType] = TypeRelevancy{RelevancyMode::Spatial, 50.0f, 5.0f};
    bool refused = false;
    try {
        ReplicationServer server(SmallServerConfig(), relevancy, schema, grid, groups, jobs,
                                 network.Server());
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    TESSERA_CHECK(refused);
    relevancy.types[kUnknownType] = TypeRelevancy{RelevancyMode::Spatial, 0.0f, 0.0f}; // never visible: fine
    ReplicationServer fine(SmallServerConfig(), relevancy, schema, grid, groups, jobs, network.Server());
    TESSERA_CHECK(fine.ConnectionCount() == 0);
}

TESSERA_TEST(server_config_defaults_for_the_budget_and_the_pacing) {
    const ServerConfig config;
    TESSERA_CHECK(config.rate_kbps == 100 && config.peak_kbps == 200 && config.max_buffer_bytes == 1100);
    TESSERA_CHECK(config.resend_margin_ticks == 1 && config.min_resend_ticks == 2);
    TESSERA_CHECK(config.max_resend_ticks == 30 && config.initial_resend_ticks == 6);
    for (const uint8_t weight : config.type_weight) {
        TESSERA_CHECK(weight == 1);
    }
}

TESSERA_TEST(the_server_names_the_budget_field_it_refuses) {
    ReplicationSchema schema = MakeTestSchema();
    SpatialGrid grid{GridConfig{64.0f, 64.0f, 64.0f}};
    RelevancyGroups groups;
    ThreadPoolJobSystem jobs(1);
    SimulatedNetwork network{NetworkConditions{}};
    auto refused_naming = [&](auto edit, const char* field) {
        ServerConfig config = SmallServerConfig();
        config.rate_kbps = 100;
        edit(config);
        try {
            ReplicationServer server(config, TestRelevancyConfig(), schema, grid, groups, jobs,
                                     network.Server());
        } catch (const std::invalid_argument& error) {
            return std::string(error.what()).find(std::string("ServerConfig::") + field) != std::string::npos;
        }
        return false;
    };
    TESSERA_CHECK(refused_naming([](ServerConfig& c) { c.rate_kbps = 1'000'001; }, "rate_kbps"));
    TESSERA_CHECK(refused_naming([](ServerConfig& c) { c.peak_kbps = 50; }, "peak_kbps"));
    TESSERA_CHECK(refused_naming([](ServerConfig& c) { c.max_buffer_bytes = 63; }, "max_buffer_bytes"));
    TESSERA_CHECK(refused_naming([](ServerConfig& c) { c.max_buffer_bytes = 65536; }, "max_buffer_bytes"));
    TESSERA_CHECK(refused_naming([](ServerConfig& c) { c.resend_margin_ticks = 31; }, "resend_margin_ticks"));
    TESSERA_CHECK(refused_naming([](ServerConfig& c) { c.min_resend_ticks = 0; }, "min_resend_ticks"));
    TESSERA_CHECK(refused_naming([](ServerConfig& c) { c.max_resend_ticks = 256; }, "max_resend_ticks"));
    TESSERA_CHECK(refused_naming(
        [](ServerConfig& c) {
            c.min_resend_ticks = 5;
            c.max_resend_ticks = 4;
        },
        "max_resend_ticks"));
    TESSERA_CHECK(refused_naming(
        [](ServerConfig& c) {
            c.min_resend_ticks = 2;
            c.initial_resend_ticks = 1; // below the minimum
        },
        "initial_resend_ticks"));
    TESSERA_CHECK(refused_naming([](ServerConfig& c) { c.type_weight[3] = 0; }, "type_weight"));
    TESSERA_CHECK(!refused_naming([](ServerConfig&) {}, "rate_kbps"));
}

TESSERA_TEST(server_passes_its_budget_settings_to_the_replication_engine) {
    ServerConfig config = SmallServerConfig();
    config.tick_rate = 60;
    config.rate_kbps = 80;
    config.peak_kbps = 160;
    config.max_buffer_bytes = 900;
    config.resend_margin_ticks = 2;
    config.min_resend_ticks = 3;
    config.max_resend_ticks = 20;
    config.initial_resend_ticks = 7;
    config.type_weight[4] = 9;
    ServerHarness h(config);
    const ReplicationConfig& engine = h.server.Replication().Config();
    TESSERA_CHECK(engine.tick_rate == 60 && engine.rate_kbps == 80 && engine.peak_kbps == 160);
    TESSERA_CHECK(engine.max_buffer_bytes == 900 && engine.resend_margin_ticks == 2);
    TESSERA_CHECK(engine.min_resend_ticks == 3 && engine.max_resend_ticks == 20 && engine.initial_resend_ticks == 7);
    TESSERA_CHECK(engine.type_weight[4] == 9 && engine.type_weight[5] == 1);
}
