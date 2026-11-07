// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tessera/net/transport/transport.hpp"

#include <cstdint>
#include <memory>
#include <random>
#include <vector>

namespace tessera::net {

/// How the simulated network treats messages.
struct NetworkConditions {
    /// Chance that one fragment of an unreliable message is lost (0 to 1). A message is lost when any of its
    /// fragments is, so a large message is lost much more often than a small one.
    double loss = 0.0;
    /// Chance that an unreliable message is delivered twice.
    double duplicate = 0.0;
    /// Every message takes between these many ticks (both included) to arrive.
    uint32_t min_delay_ticks = 0;
    uint32_t max_delay_ticks = 0;
    /// Bytes per datagram; a message of `n` bytes is `ceil(n / mtu)` fragments.
    uint32_t mtu = 1200;
    /// Seed of the random numbers. Read when the network is created.
    uint32_t seed = 1;
};

/// A server and any number of clients connected through memory, for tests and benchmarks. Nothing depends on
/// a real clock: time moves only when `Advance` is called, and the same seed with the same traffic always
/// gives the same result.
///
/// It models, rather than reproduces, what a real network does. Reliable messages always arrive, in order,
/// only later. An unreliable message is lost if any of its fragments is and can arrive late, out of order or
/// twice. A real transport such as ENet also retransmits and may behave differently under heavy loss.
class SimulatedNetwork {
public:
    explicit SimulatedNetwork(const NetworkConditions& conditions);
    ~SimulatedNetwork();

    SimulatedNetwork(const SimulatedNetwork&) = delete;
    SimulatedNetwork& operator=(const SimulatedNetwork&) = delete;

    /// The server's end. It sees each client as a connection with an id that is never reused.
    ITransport& Server() noexcept;

    /// Adds a client and returns its end, which stays valid as long as the network does. Both sides see
    /// `Connected` on the next `Advance`.
    ITransport* AddClient();

    /// Moves time to `tick` and makes everything that was due by then available to `Poll`.
    void Advance(uint64_t tick);

    /// Changes how messages sent from now on are treated (the seed is not read again).
    void SetConditions(const NetworkConditions& conditions);

    /// Messages that were sent and have not been delivered or dropped yet.
    size_t InFlight() const noexcept {
        return flight_.size();
    }

private:
    class Endpoint;
    friend class Endpoint;

    // One client's connection to the server.
    struct Link {
        bool client_closed = false;         // the client called Disconnect
        bool server_closed = false;         // the server called Disconnect
        bool client_notified = false;       // the client has been told the server closed
        bool server_notified = false;       // the server has been told the client closed
        uint64_t last_reliable[2] = {0, 0}; // delivery time of the last reliable item, [0] toward the server
    };

    struct Item {
        uint64_t deliver_at;
        uint64_t sequence;
        uint32_t link;
        bool to_server;
        TransportEvent::Kind kind;
        Channel channel;
        std::vector<uint8_t> data;
    };

    bool SendFrom(bool from_server, uint32_t link, Channel channel, std::span<const uint8_t> data,
                  TransportStats& stats);
    void DisconnectFrom(bool from_server, uint32_t link);
    uint32_t RollDelay();
    double Uniform();
    void Queue(uint64_t deliver_at, uint32_t link, bool to_server, TransportEvent::Kind kind, Channel channel,
               std::vector<uint8_t> data);
    uint64_t ReliableDelivery(Link& link, bool to_server);

    NetworkConditions conditions_;
    std::mt19937_64 random_;
    uint64_t now_ = 0;
    uint64_t next_sequence_ = 0;
    std::vector<Link> links_;
    std::vector<Item> flight_;
    std::unique_ptr<Endpoint> server_;
    std::vector<std::unique_ptr<Endpoint>> clients_;
};

} // namespace tessera::net
