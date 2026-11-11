// SPDX-License-Identifier: Apache-2.0
// The ENet transport over loopback sockets. These tests use real time (ENet runs on the system clock), so the
// waits are generous and a test that cannot reach its goal fails instead of hanging.
#include "tessera/net/client_replica.hpp"
#include "tessera/net/entity_table.hpp"
#include "tessera/net/job_system.hpp"
#include "tessera/net/replication_server.hpp"
#include "tessera/net/spatial_grid.hpp"
#include "tessera/net/transport/enet_transport.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <enet/enet.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace tessera::net;

namespace {

using Clock = std::chrono::steady_clock;

// A server and one client connected to it on this machine.
struct Pair {
    EnetServerTransport server;
    EnetClientTransport client;
    std::vector<TransportEvent> server_events;
    std::vector<TransportEvent> client_events;

    Pair() : server(EnetServerConfig{}), client(EnetClientConfig{"127.0.0.1", server.Port(), 5000}) {}
};

// Runs both transports until `done` is true or `limit` has passed. Returns whether `done` became true; the
// callers check it, so a test that cannot get where it is going fails instead of hanging.
template <class Predicate>
bool PumpUntil(EnetServerTransport& server, EnetClientTransport& client,
               std::vector<TransportEvent>& server_events, std::vector<TransportEvent>& client_events,
               std::chrono::milliseconds limit, Predicate done) {
    const auto deadline = Clock::now() + limit;
    while (Clock::now() < deadline) {
        server.Poll(server_events);
        client.Poll(client_events);
        server.Flush();
        client.Flush();
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

template <class Predicate> bool PumpUntil(Pair& pair, std::chrono::milliseconds limit, Predicate done) {
    return PumpUntil(pair.server, pair.client, pair.server_events, pair.client_events, limit, done);
}

// Runs for a fixed time, whatever happens.
void PumpFor(Pair& pair, std::chrono::milliseconds time) {
    PumpUntil(pair, time, [] { return false; });
}

size_t CountKind(const std::vector<TransportEvent>& events, TransportEvent::Kind kind) {
    return static_cast<size_t>(std::count_if(
        events.begin(), events.end(), [&](const TransportEvent& event) { return event.kind == kind; }));
}

bool Connected(const Pair& pair) {
    return CountKind(pair.server_events, TransportEvent::Kind::Connected) == 1 &&
           CountKind(pair.client_events, TransportEvent::Kind::Connected) == 1;
}

// Connects the pair, or fails the calling test.
bool Connect(Pair& pair) {
    const bool ok = PumpUntil(pair, std::chrono::seconds(5), [&] { return Connected(pair); });
    TESSERA_CHECK(ok);
    return ok;
}

// A message that starts with a number and is `size` bytes long (at least 4).
std::vector<uint8_t> Numbered(uint32_t number, size_t size = 24) {
    std::vector<uint8_t> bytes(std::max<size_t>(size, 4), static_cast<uint8_t>(number * 7u + 3u));
    for (int i = 0; i < 4; ++i) {
        bytes[static_cast<size_t>(i)] = static_cast<uint8_t>(number >> (8 * i));
    }
    return bytes;
}

uint32_t NumberOf(const std::vector<uint8_t>& bytes) {
    uint32_t number = 0;
    for (int i = 0; i < 4 && static_cast<size_t>(i) < bytes.size(); ++i) {
        number |= static_cast<uint32_t>(bytes[static_cast<size_t>(i)]) << (8 * i);
    }
    return number;
}

std::vector<uint8_t> Pattern(size_t size, uint8_t seed) {
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<uint8_t>(seed + i * 31u + (i >> 8));
    }
    return bytes;
}

std::vector<const TransportEvent*> Messages(const std::vector<TransportEvent>& events) {
    std::vector<const TransportEvent*> found;
    for (const TransportEvent& event : events) {
        if (event.kind == TransportEvent::Kind::Message) {
            found.push_back(&event);
        }
    }
    return found;
}

// Lets a test drop the datagrams a host receives (ENet calls this before it reads them).
std::atomic<int> g_datagrams_to_drop{0};
std::atomic<bool> g_drop_everything{false};

int DropSome(ENetHost*, ENetEvent*) {
    int left = g_datagrams_to_drop.load();
    while (left > 0) {
        if (g_datagrams_to_drop.compare_exchange_weak(left, left - 1)) {
            return 1; // dealt with: ENet does not read it
        }
    }
    return 0;
}

int DropAll(ENetHost*, ENetEvent*) {
    return g_drop_everything.load() ? 1 : 0;
}

} // namespace

TESSERA_TEST(enet_client_connects_and_both_sides_see_it) {
    Pair pair;
    TESSERA_CHECK(pair.server.Port() != 0);
    if (!Connect(pair)) {
        return;
    }
    TESSERA_CHECK(pair.server_events.size() == 1);
    TESSERA_CHECK(pair.client_events.size() == 1);
    TESSERA_CHECK(pair.server_events[0].kind == TransportEvent::Kind::Connected);
    TESSERA_CHECK(pair.server_events[0].connection == 1); // ids start at 1
    TESSERA_CHECK(pair.client_events[0].kind == TransportEvent::Kind::Connected);
    TESSERA_CHECK(pair.client_events[0].connection == 0); // the server is connection 0
}

TESSERA_TEST(enet_messages_arrive_in_both_directions) {
    Pair pair;
    if (!Connect(pair)) {
        return;
    }
    const std::vector<uint8_t> up = Pattern(40, 1);
    const std::vector<uint8_t> down = Pattern(70, 2);
    TESSERA_CHECK(pair.client.Send(0, Channel::ReliableOrdered, up));
    TESSERA_CHECK(pair.server.Send(1, Channel::ReliableOrdered, down));
    const bool ok = PumpUntil(pair, std::chrono::seconds(5), [&] {
        return !Messages(pair.server_events).empty() && !Messages(pair.client_events).empty();
    });
    TESSERA_CHECK(ok);
    if (ok) {
        const auto at_server = Messages(pair.server_events);
        const auto at_client = Messages(pair.client_events);
        TESSERA_CHECK(at_server.size() == 1 && at_server[0]->data == up);
        TESSERA_CHECK(at_server[0]->connection == 1);
        TESSERA_CHECK(at_server[0]->channel == Channel::ReliableOrdered);
        TESSERA_CHECK(at_client.size() == 1 && at_client[0]->data == down);
        TESSERA_CHECK(at_client[0]->connection == 0);
    }
}

TESSERA_TEST(enet_reliable_messages_arrive_all_and_in_order) {
    Pair pair;
    if (!Connect(pair)) {
        return;
    }
    constexpr uint32_t kCount = 2000;
    for (uint32_t i = 0; i < kCount; ++i) {
        TESSERA_CHECK(pair.server.Send(1, Channel::ReliableOrdered, Numbered(i)));
        TESSERA_CHECK(pair.client.Send(0, Channel::ReliableOrdered, Numbered(i)));
    }
    const bool ok = PumpUntil(pair, std::chrono::seconds(10), [&] {
        return Messages(pair.server_events).size() >= kCount && Messages(pair.client_events).size() >= kCount;
    });
    TESSERA_CHECK(ok);
    for (const auto* events : {&pair.server_events, &pair.client_events}) {
        const auto messages = Messages(*events);
        TESSERA_CHECK(messages.size() == kCount);
        bool in_order = true;
        for (size_t i = 0; i < messages.size() && i < kCount; ++i) {
            in_order =
                in_order && NumberOf(messages[i]->data) == i && messages[i]->data == Numbered(uint32_t(i));
        }
        TESSERA_CHECK(in_order);
    }
}

TESSERA_TEST(enet_reliable_messages_survive_dropped_datagrams) {
    Pair pair;
    if (!Connect(pair)) {
        return;
    }
    auto* host = static_cast<ENetHost*>(pair.client.NativeHost());
    TESSERA_CHECK(host != nullptr);
    if (host == nullptr) {
        return;
    }
    g_datagrams_to_drop = 3;
    host->intercept = DropSome;
    constexpr uint32_t kCount = 200;
    for (uint32_t i = 0; i < kCount; ++i) {
        TESSERA_CHECK(pair.server.Send(1, Channel::ReliableOrdered, Numbered(i, 200)));
    }
    const bool ok = PumpUntil(pair, std::chrono::seconds(5),
                              [&] { return Messages(pair.client_events).size() >= kCount; });
    host->intercept = nullptr;
    TESSERA_CHECK(ok);
    TESSERA_CHECK(g_datagrams_to_drop.load() == 0); // the datagrams really were dropped
    const auto messages = Messages(pair.client_events);
    TESSERA_CHECK(messages.size() == kCount);
    bool in_order = true;
    for (size_t i = 0; i < messages.size() && i < kCount; ++i) {
        in_order = in_order && messages[i]->data == Numbered(uint32_t(i), 200);
    }
    TESSERA_CHECK(in_order);
}

TESSERA_TEST(enet_large_message_arrives_whole_on_both_channels) {
    Pair pair;
    if (!Connect(pair)) {
        return;
    }
    constexpr size_t kSize = 50000;
    const Channel channels[] = {Channel::ReliableOrdered, Channel::Unreliable};
    for (const Channel channel : channels) {
        const std::vector<uint8_t> down = Pattern(kSize, channel == Channel::Unreliable ? 5 : 6);
        const std::vector<uint8_t> up = Pattern(kSize, channel == Channel::Unreliable ? 7 : 8);
        pair.server_events.clear();
        pair.client_events.clear();
        TESSERA_CHECK(pair.server.Send(1, channel, down));
        TESSERA_CHECK(pair.client.Send(0, channel, up));
        const bool ok = PumpUntil(pair, std::chrono::seconds(5), [&] {
            return !Messages(pair.server_events).empty() && !Messages(pair.client_events).empty();
        });
        TESSERA_CHECK(ok);
        if (!ok) {
            continue;
        }
        const auto at_server = Messages(pair.server_events);
        const auto at_client = Messages(pair.client_events);
        TESSERA_CHECK(at_server.size() == 1 && at_server[0]->data == up && at_server[0]->channel == channel);
        TESSERA_CHECK(at_client.size() == 1 && at_client[0]->data == down &&
                      at_client[0]->channel == channel);
    }
}

TESSERA_TEST(enet_unreliable_messages_arrive_on_loopback) {
    Pair pair;
    if (!Connect(pair)) {
        return;
    }
    constexpr uint32_t kCount = 100;
    for (uint32_t i = 0; i < kCount; ++i) {
        TESSERA_CHECK(pair.server.Send(1, Channel::Unreliable, Numbered(i, 60)));
    }
    PumpUntil(pair, std::chrono::seconds(3), [&] { return Messages(pair.client_events).size() >= kCount; });
    const auto messages = Messages(pair.client_events);
    TESSERA_CHECK(messages.size() >= 95);
    bool whole = true;
    for (const auto* message : messages) {
        whole = whole && message->channel == Channel::Unreliable &&
                message->data == Numbered(NumberOf(message->data), 60);
    }
    TESSERA_CHECK(whole);
}

TESSERA_TEST(enet_unreliable_message_is_not_retransmitted) {
    Pair pair;
    if (!Connect(pair)) {
        return;
    }
    auto* host = static_cast<ENetHost*>(pair.client.NativeHost());
    TESSERA_CHECK(host != nullptr);
    if (host == nullptr) {
        return;
    }
    g_drop_everything = true;
    host->intercept = DropAll;
    // A message of ~50 KB is split into fragments. Sent as an unreliable message, losing them loses it; if
    // the transport let ENet send them as reliable fragments, they would be sent again and arrive.
    TESSERA_CHECK(pair.server.Send(1, Channel::Unreliable, Pattern(50000, 9)));
    PumpFor(pair, std::chrono::milliseconds(300));
    g_drop_everything = false;
    PumpFor(pair, std::chrono::seconds(2));
    host->intercept = nullptr;
    TESSERA_CHECK(Messages(pair.client_events).empty());
    TESSERA_CHECK(CountKind(pair.client_events, TransportEvent::Kind::Disconnected) == 0); // still connected
}

TESSERA_TEST(enet_send_to_unknown_or_closed_connection_returns_false) {
    const std::vector<uint8_t> bytes = Pattern(10, 1);
    {
        EnetServerTransport server(EnetServerConfig{});
        EnetClientTransport client(EnetClientConfig{"127.0.0.1", server.Port(), 5000});
        // The client has asked for the connection but it is not open yet.
        TESSERA_CHECK(!client.Send(0, Channel::ReliableOrdered, bytes));
    }
    Pair pair;
    if (!Connect(pair)) {
        return;
    }
    TESSERA_CHECK(!pair.server.Send(999, Channel::ReliableOrdered, bytes));
    TESSERA_CHECK(!pair.server.Send(0, Channel::ReliableOrdered, bytes)); // the server has no connection 0
    TESSERA_CHECK(!pair.client.Send(5, Channel::Unreliable, bytes));
    TESSERA_CHECK(pair.server.Send(1, Channel::ReliableOrdered, bytes));
    pair.server.Disconnect(1);
    TESSERA_CHECK(!pair.server.Send(1, Channel::ReliableOrdered, bytes));
    TESSERA_CHECK(!pair.server.Send(1, Channel::Unreliable, bytes));
    // Once the other side has gone, the connection is closed for the one that stayed.
    PumpUntil(pair, std::chrono::seconds(5),
              [&] { return CountKind(pair.client_events, TransportEvent::Kind::Disconnected) == 1; });
    TESSERA_CHECK(!pair.client.Send(0, Channel::ReliableOrdered, bytes));
}

TESSERA_TEST(enet_connection_ids_are_never_reused) {
    EnetServerTransport server(EnetServerConfig{});
    std::vector<TransportEvent> server_events;
    std::vector<TransportConnection> ids;
    for (int round = 0; round < 3; ++round) {
        std::vector<TransportEvent> client_events;
        EnetClientTransport client(EnetClientConfig{"127.0.0.1", server.Port(), 5000});
        const size_t first = server_events.size();
        const bool connected = PumpUntil(
            server, client, server_events, client_events, std::chrono::seconds(5), [&] {
                return CountKind(client_events, TransportEvent::Kind::Connected) == 1 &&
                       CountKind(server_events, TransportEvent::Kind::Connected) ==
                           static_cast<size_t>(round + 1);
            });
        TESSERA_CHECK(connected);
        for (size_t i = first; i < server_events.size(); ++i) {
            if (server_events[i].kind == TransportEvent::Kind::Connected) {
                ids.push_back(server_events[i].connection);
            }
        }
        client.Disconnect(0);
        client.Flush();
        const bool gone =
            PumpUntil(server, client, server_events, client_events, std::chrono::seconds(5), [&] {
                return CountKind(server_events, TransportEvent::Kind::Disconnected) ==
                       static_cast<size_t>(round + 1);
            });
        TESSERA_CHECK(gone);
    }
    TESSERA_CHECK(ids.size() == 3);
    if (ids.size() == 3) {
        TESSERA_CHECK(ids[0] == 1 && ids[1] == 2 && ids[2] == 3);
    }
}

TESSERA_TEST(enet_stats_count_bytes_and_messages) {
    Pair pair;
    if (!Connect(pair)) {
        return;
    }
    TESSERA_CHECK(pair.server.Stats().messages_sent == 0 && pair.client.Stats().messages_received == 0);
    for (int i = 0; i < 10; ++i) {
        TESSERA_CHECK(pair.client.Send(0, Channel::ReliableOrdered, Pattern(100, 1)));
    }
    for (int i = 0; i < 5; ++i) {
        TESSERA_CHECK(pair.client.Send(0, Channel::Unreliable, Pattern(60, 2)));
    }
    for (int i = 0; i < 7; ++i) {
        TESSERA_CHECK(pair.server.Send(1, Channel::ReliableOrdered, Pattern(30, 3)));
    }
    const bool ok = PumpUntil(pair, std::chrono::seconds(5), [&] {
        return Messages(pair.server_events).size() >= 15 && Messages(pair.client_events).size() >= 7;
    });
    TESSERA_CHECK(ok);
    const TransportStats client = pair.client.Stats();
    const TransportStats server = pair.server.Stats();
    TESSERA_CHECK(client.messages_sent == 15 && client.bytes_sent == 10 * 100 + 5 * 60);
    TESSERA_CHECK(server.messages_received == 15 && server.bytes_received == 10 * 100 + 5 * 60);
    TESSERA_CHECK(server.messages_sent == 7 && server.bytes_sent == 7 * 30);
    TESSERA_CHECK(client.messages_received == 7 && client.bytes_received == 7 * 30);
    TESSERA_CHECK(server.messages_lost == 0 && client.messages_lost == 0);
    // The sockets carried at least the messages, and the sent bytes of one side were read by the other.
    const EnetWireStats wire_client = pair.client.Wire();
    const EnetWireStats wire_server = pair.server.Wire();
    TESSERA_CHECK(wire_client.bytes_sent >= client.bytes_sent && wire_server.bytes_sent >= server.bytes_sent);
    TESSERA_CHECK(wire_server.bytes_received >= server.bytes_received);
    TESSERA_CHECK(wire_server.bytes_received <= wire_client.bytes_sent);
}

TESSERA_TEST(enet_disconnect_delivers_earlier_messages_first) {
    Pair pair;
    if (!Connect(pair)) {
        return;
    }
    constexpr uint32_t kCount = 100;
    for (uint32_t i = 0; i < kCount; ++i) {
        TESSERA_CHECK(pair.server.Send(1, Channel::ReliableOrdered, Numbered(i, 100)));
    }
    pair.server.Disconnect(1);
    const bool ok = PumpUntil(pair, std::chrono::seconds(5), [&] {
        return CountKind(pair.client_events, TransportEvent::Kind::Disconnected) == 1;
    });
    TESSERA_CHECK(ok);
    const auto messages = Messages(pair.client_events);
    TESSERA_CHECK(messages.size() == kCount);
    TESSERA_CHECK(!pair.client_events.empty() &&
                  pair.client_events.back().kind == TransportEvent::Kind::Disconnected);
    TESSERA_CHECK(pair.client_events.size() == kCount + 2); // Connected, the messages, Disconnected
}

TESSERA_TEST(enet_the_side_that_disconnects_sees_nothing_and_cannot_send) {
    Pair pair;
    if (!Connect(pair)) {
        return;
    }
    // Messages the client sent before the server closes the connection are still on their way.
    for (uint32_t i = 0; i < 5; ++i) {
        TESSERA_CHECK(pair.client.Send(0, Channel::ReliableOrdered, Numbered(i)));
    }
    pair.client.Flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    pair.server.Disconnect(1);
    TESSERA_CHECK(!pair.server.Send(1, Channel::ReliableOrdered, Numbered(1)));
    const size_t server_events_before = pair.server_events.size();
    PumpFor(pair, std::chrono::milliseconds(600));
    TESSERA_CHECK(pair.server_events.size() == server_events_before); // no messages, no Disconnected
    TESSERA_CHECK(CountKind(pair.client_events, TransportEvent::Kind::Disconnected) == 1);
    // The same from the client's side.
    Pair other;
    if (!Connect(other)) {
        return;
    }
    other.client.Disconnect(0);
    TESSERA_CHECK(!other.client.Send(0, Channel::ReliableOrdered, Numbered(2)));
    PumpFor(other, std::chrono::milliseconds(600));
    TESSERA_CHECK(CountKind(other.client_events, TransportEvent::Kind::Disconnected) == 0);
    TESSERA_CHECK(Messages(other.client_events).empty());
    TESSERA_CHECK(CountKind(other.server_events, TransportEvent::Kind::Disconnected) == 1);
}

TESSERA_TEST(enet_client_that_cannot_connect_is_told_it_disconnected) {
    uint16_t port = 0;
    {
        EnetServerTransport server(EnetServerConfig{});
        port = server.Port();
    } // nobody listens on it any more
    EnetClientTransport client(EnetClientConfig{"127.0.0.1", port, 1000});
    std::vector<TransportEvent> events;
    const auto deadline = Clock::now() + std::chrono::seconds(4);
    while (Clock::now() < deadline && CountKind(events, TransportEvent::Kind::Disconnected) == 0) {
        client.Poll(events);
        client.Flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TESSERA_CHECK(CountKind(events, TransportEvent::Kind::Disconnected) == 1);
    TESSERA_CHECK(CountKind(events, TransportEvent::Kind::Connected) == 0);
    TESSERA_CHECK(!client.Send(0, Channel::ReliableOrdered, Pattern(4, 1)));
}

TESSERA_TEST(enet_replication_converges_over_enet) {
    struct Columns {
        float x[kPageSlots];
        float y[kPageSlots];
        float z[kPageSlots];
    };
    ReplicationSchema schema;
    schema.AddType(0, {PropertyDescriptor::Position(TESSERA_COLUMN(Columns, x), TESSERA_COLUMN(Columns, y),
                                                    TESSERA_COLUMN(Columns, z), 0.01f, 2048.0f)});
    EntityTable<Columns> table(TableConfig{0, 0});
    SpatialGrid grid(GridConfig{64.0f, 64.0f, 64.0f});
    RelevancyConfig relevancy;
    relevancy.types[0] = TypeRelevancy{RelevancyMode::AlwaysRelevant, 0.0f, 0.0f};
    ThreadPoolJobSystem jobs(1);
    RelevancyGroups groups;
    ServerConfig config;
    config.max_connections = 4;
    config.max_visible_per_connection = 1024;
    config.tick_rate = 20;
    config.rate_kbps = 0;
    config.max_buffer_bytes = 65535;

    EnetServerTransport server_transport(EnetServerConfig{});
    ReplicationServer server(config, relevancy, schema, grid, groups, jobs, server_transport);
    constexpr int kEntities = 50;
    std::vector<EntityId> entities;
    for (int i = 0; i < kEntities; ++i) {
        const EntityId id = table.Create();
        const auto where = table.Find(id);
        Columns& columns = table.Columns(where->page);
        columns.x[where->slot] = static_cast<float>(i);
        columns.y[where->slot] = 0.0f;
        columns.z[where->slot] = static_cast<float>(-i);
        table.MarkDirty(id);
        grid.Insert(id, static_cast<float>(i), 0.0f, static_cast<float>(-i));
        server.Relevancy().OnEntityCreated(id);
        entities.push_back(id);
    }

    EnetClientTransport transport_a(EnetClientConfig{"127.0.0.1", server_transport.Port(), 5000});
    EnetClientTransport transport_b(EnetClientConfig{"127.0.0.1", server_transport.Port(), 5000});
    ClientReplica a(transport_a, schema);
    ClientReplica b(transport_b, schema);

    auto converged = [&](ClientReplica& client) {
        if (client.State() != ClientState::Connected || client.VisibleCount() != kEntities) {
            return false;
        }
        for (int i = 0; i < kEntities; ++i) {
            const uint32_t* words = client.WordsOf(entities[static_cast<size_t>(i)]);
            if (words == nullptr) {
                return false;
            }
            const int64_t expected_x = (static_cast<int64_t>(i) + 2048) * 100;
            const int64_t expected_z = (static_cast<int64_t>(-i) + 2048) * 100;
            if (std::abs(static_cast<int64_t>(words[0]) - expected_x) > 1 ||
                std::abs(static_cast<int64_t>(words[2]) - expected_z) > 1) {
                return false;
            }
        }
        return true;
    };

    const auto deadline = Clock::now() + std::chrono::seconds(5);
    uint64_t tick = 0;
    bool ok = false;
    while (Clock::now() < deadline && !ok) {
        a.Poll();
        b.Poll();
        transport_a.Flush();
        transport_b.Flush();
        server.Refresh(table);
        server.Update(tick++);
        server_transport.Flush();
        ok = converged(a) && converged(b);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    TESSERA_CHECK(ok);
    TESSERA_CHECK(server.ConnectionCount() == 2);
}

TESSERA_TEST(enet_destroyed_client_is_seen_by_the_server_as_disconnected) {
    EnetServerTransport server(EnetServerConfig{});
    auto client = std::make_unique<EnetClientTransport>(EnetClientConfig{"127.0.0.1", server.Port(), 5000});
    std::vector<TransportEvent> server_events;
    std::vector<TransportEvent> client_events;
    const bool connected =
        PumpUntil(server, *client, server_events, client_events, std::chrono::seconds(5),
                  [&] { return CountKind(server_events, TransportEvent::Kind::Connected) == 1; });
    TESSERA_CHECK(connected);
    client.reset();
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < deadline && CountKind(server_events, TransportEvent::Kind::Disconnected) == 0) {
        server.Poll(server_events);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TESSERA_CHECK(CountKind(server_events, TransportEvent::Kind::Disconnected) == 1);
    TESSERA_CHECK(!server.Send(1, Channel::ReliableOrdered, Pattern(4, 1)));
}
