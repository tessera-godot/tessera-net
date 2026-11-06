// SPDX-License-Identifier: Apache-2.0
// A replication server on a simulated network, with raw fake clients that the test drives by hand.
#pragma once

#include "network_test_support.hpp"
#include "tessera/net/replication_server.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace network_test {

inline ServerConfig SmallServerConfig(uint32_t max_connections = 4) {
    ServerConfig config;
    config.max_connections = max_connections;
    config.max_visible_per_connection = 1000;
    return config;
}

inline RelevancyConfig TestRelevancyConfig() {
    RelevancyConfig config;
    config.max_connections = 2; // the server overrides this
    config.max_visible_per_connection = 10;
    config.types[kMobType] = TypeRelevancy{RelevancyMode::Spatial, 150.0f, 10.0f};
    return config;
}

/// A client end that is only a transport: the test sends whatever bytes it wants and reads what the server
/// sent.
struct FakeClient {
    ITransport* transport;
    TransportConnection connection = 0;
    std::vector<TransportEvent> inbox;
    bool disconnected = false;

    void Collect() {
        const size_t before = inbox.size();
        transport->Poll(inbox);
        for (size_t i = before; i < inbox.size(); ++i) {
            if (inbox[i].kind == TransportEvent::Kind::Disconnected) {
                disconnected = true;
            }
        }
    }
    bool Send(Channel channel, const std::vector<uint8_t>& message) {
        return transport->Send(0, channel, message);
    }
    void SendHello(uint32_t version, uint64_t hash) {
        std::vector<uint8_t> bytes;
        Encode(ClientHello{version, hash}, bytes);
        Send(Channel::ReliableOrdered, bytes);
    }
    void SendAck(uint64_t latest, uint32_t mask) {
        std::vector<uint8_t> bytes;
        Encode(Ack{latest, mask}, bytes);
        Send(Channel::Unreliable, bytes);
    }

    std::vector<ServerHello> Hellos() const {
        std::vector<ServerHello> found;
        for (const TransportEvent& event : inbox) {
            ServerHello hello{};
            if (event.kind == TransportEvent::Kind::Message && Decode(event.data, &hello)) {
                found.push_back(hello);
            }
        }
        return found;
    }
    std::vector<EventBatch> Batches() const {
        std::vector<EventBatch> found;
        for (const TransportEvent& event : inbox) {
            EventBatch batch;
            if (event.kind == TransportEvent::Kind::Message && Decode(event.data, &batch)) {
                found.push_back(std::move(batch));
            }
        }
        return found;
    }
    struct Buffer {
        StateBufferHeader header;
        std::vector<uint8_t> payload;
    };
    std::vector<Buffer> Buffers() const {
        std::vector<Buffer> found;
        for (const TransportEvent& event : inbox) {
            StateBufferHeader header{};
            std::span<const uint8_t> payload;
            if (event.kind == TransportEvent::Kind::Message &&
                DecodeStateBuffer(event.data, &header, &payload)) {
                found.push_back(Buffer{header, std::vector<uint8_t>(payload.begin(), payload.end())});
            }
        }
        return found;
    }
};

struct ServerHarness {
    ReplicationSchema schema = MakeTestSchema();
    SpatialGrid grid{GridConfig{64.0f, 64.0f, 64.0f}};
    RelevancyGroups groups;
    ThreadPoolJobSystem jobs;
    SimulatedNetwork network;
    ReplicationServer server;
    std::vector<std::unique_ptr<FakeClient>> clients;
    std::vector<ConnectionId> connected;
    std::vector<ConnectionId> disconnected;
    uint64_t tick = 0;

    explicit ServerHarness(const ServerConfig& config = SmallServerConfig(),
                           const NetworkConditions& conditions = NetworkConditions{}, uint32_t workers = 1)
        : jobs(workers), network(conditions),
          server(config, TestRelevancyConfig(), schema, grid, groups, jobs, network.Server()) {
        server.SetCallbacks([this](ConnectionId id) { connected.push_back(id); },
                            [this](ConnectionId id) { disconnected.push_back(id); });
    }

    FakeClient& AddClient() {
        clients.push_back(std::make_unique<FakeClient>());
        clients.back()->transport = network.AddClient();
        return *clients.back();
    }

    // One tick: the network delivers, the fake clients read, the server runs its network phase.
    void Step() {
        network.Advance(tick);
        for (const auto& client : clients) {
            client->Collect();
        }
        server.Update(tick);
        ++tick;
    }

    // A fake client that connects and is accepted, ready for the test to use.
    FakeClient& ConnectAndGreet() {
        FakeClient& client = AddClient();
        Step();
        client.SendHello(kProtocolVersion, schema.Hash());
        Step();
        Step();
        return client;
    }
};

} // namespace network_test
