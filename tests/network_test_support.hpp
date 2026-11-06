// SPDX-License-Identifier: Apache-2.0
// Shared pieces for the network tests: a client wired to a simulated network, with the server end driven by
// hand.
#pragma once

#include "replication_test_support.hpp"
#include "tessera/net/client_replica.hpp"
#include "tessera/net/protocol.hpp"
#include "tessera/net/transport/simulated_network.hpp"

#include <cstdint>
#include <vector>

namespace network_test {

using namespace tessera::net;
using namespace replication_test;

/// One `ClientReplica` on a simulated network, and a server end that the test drives message by message.
struct ClientHarness {
    ReplicationSchema schema = MakeTestSchema();
    SimulatedNetwork network;
    ITransport* transport;
    ClientReplica client;
    TransportConnection connection = 0; // the id the server end gives this client, known after the first step
    std::vector<TransportEvent> from_client; // everything the server end received from the client
    uint64_t tick = 0;

    explicit ClientHarness(const NetworkConditions& conditions = NetworkConditions{})
        : network(conditions), transport(network.AddClient()), client(*transport, schema) {}

    // One tick: the network delivers what is due, the server end collects it, the client reads its transport.
    void Step() {
        network.Advance(tick++);
        const size_t before = from_client.size();
        network.Server().Poll(from_client);
        for (size_t i = before; i < from_client.size(); ++i) {
            if (from_client[i].kind == TransportEvent::Kind::Connected) {
                connection = from_client[i].connection;
            }
        }
        client.Poll();
    }

    // Server to client.
    bool Send(Channel channel, const std::vector<uint8_t>& message) {
        return network.Server().Send(connection, channel, message);
    }

    void SendHello(const ServerHello& hello) {
        std::vector<uint8_t> bytes;
        Encode(hello, bytes);
        Send(Channel::ReliableOrdered, bytes);
    }

    void Accept(uint32_t connection_id = 3, uint32_t max_visible = 1000, uint32_t baseline_age = 32) {
        SendHello(ServerHello{true, RejectReason::None, kProtocolVersion, connection_id, 30, max_visible,
                              baseline_age});
    }

    void Reject(RejectReason reason) {
        SendHello(ServerHello{false, reason, 0, 0, 0, 0, 0});
    }

    void SendBatch(uint64_t number, uint64_t batch_tick, std::vector<VisibilityEvent> events) {
        std::vector<uint8_t> bytes;
        Encode(EventBatch{number, batch_tick, std::move(events)}, bytes);
        Send(Channel::ReliableOrdered, bytes);
    }

    void SendStateBuffer(uint64_t buffer_tick, uint64_t batches_sent, const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> bytes;
        EncodeStateBuffer(StateBufferHeader{buffer_tick, batches_sent}, payload, bytes);
        Send(Channel::Unreliable, bytes);
    }

    // The messages of one kind the client has sent, decoded.
    std::vector<ClientHello> Hellos() const {
        std::vector<ClientHello> found;
        for (const TransportEvent& event : from_client) {
            ClientHello hello{};
            if (event.kind == TransportEvent::Kind::Message && Decode(event.data, &hello)) {
                found.push_back(hello);
            }
        }
        return found;
    }

    std::vector<Ack> Acks() const {
        std::vector<Ack> found;
        for (const TransportEvent& event : from_client) {
            Ack ack{};
            if (event.kind == TransportEvent::Kind::Message && Decode(event.data, &ack)) {
                found.push_back(ack);
            }
        }
        return found;
    }

    size_t MessagesFromClient() const {
        size_t count = 0;
        for (const TransportEvent& event : from_client) {
            count += event.kind == TransportEvent::Kind::Message ? 1 : 0;
        }
        return count;
    }
};

inline VisibilityEvent Enter(EntityId id, uint32_t compact_id) {
    return VisibilityEvent{VisibilityEvent::Kind::Enter, id, compact_id};
}
inline VisibilityEvent Leave(EntityId id, uint32_t compact_id) {
    return VisibilityEvent{VisibilityEvent::Kind::Leave, id, compact_id};
}
inline VisibilityEvent Destroy(EntityId id, uint32_t compact_id) {
    return VisibilityEvent{VisibilityEvent::Kind::Destroy, id, compact_id};
}

} // namespace network_test
