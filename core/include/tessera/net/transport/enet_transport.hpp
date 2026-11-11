// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tessera/net/transport/transport.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace tessera::net {

// A transport over real UDP sockets, built on ENet (https://github.com/lsalzman/enet). It exists only when
// the project is configured with -DTESSERA_BUILD_ENET=ON, and the ENet headers stay out of this file.
//
// Channels: a reliable message goes on ENet channel 0 as a reliable packet, so it arrives, once and in order.
// An unreliable message goes on ENet channel 1 as an unreliable packet that may be split into fragments; if
// one fragment is lost the whole message is lost, and nothing is sent again (except that ENet 1.3.18 sends
// one unreliable packet in every 65,536 on a channel as a reliable one, at the rollover of its sequence
// number).
//
// A peer that goes silent is detected with ENet's default timeouts (1.3.18: a limit of 32, a minimum of 5,000
// ms and a maximum of 30,000 ms; the client's follow `timeout_ms`). `Poll`, `Send` and `Flush` must be called
// from one thread.
//
// `Stats` counts what `Send` accepted and what `Poll` handed over, in message bytes. ENet does not say which
// message a lost packet carried, so `messages_lost` is always 0 here; `Wire` has the bytes that went through
// the sockets.

struct EnetServerConfig {
    /// The address to listen on; empty means every interface.
    std::string bind_address = "127.0.0.1";
    /// The port to listen on; 0 lets the system choose (see `EnetServerTransport::Port`).
    uint16_t port = 0;
    /// Most clients at once, 1 to 4,095 (the most ENet allows).
    uint32_t max_clients = 128;
};

struct EnetClientConfig {
    std::string server_address = "127.0.0.1";
    uint16_t server_port = 0;
    /// How long without an answer before the connection is dropped, also while it is being opened.
    uint32_t timeout_ms = 5000;
};

/// Everything ENet put on the sockets and read from them, its own headers included (UDP and IP headers are
/// not). The same quantity as ENet's `totalSentData` and `totalReceivedData`, extended to 64 bits.
struct EnetWireStats {
    uint64_t bytes_sent = 0;
    uint64_t bytes_received = 0;
};

/// The server end. Each client is seen under an id that starts at 1 and is never used again.
class EnetServerTransport final : public ITransport {
public:
    /// Throws std::runtime_error, with the cause, if the socket cannot be opened (for instance a port that is
    /// in use).
    explicit EnetServerTransport(const EnetServerConfig& config);
    ~EnetServerTransport() override;
    EnetServerTransport(const EnetServerTransport&) = delete;
    EnetServerTransport& operator=(const EnetServerTransport&) = delete;
    EnetServerTransport(EnetServerTransport&&) = delete;
    EnetServerTransport& operator=(EnetServerTransport&&) = delete;

    /// The port the server listens on.
    uint16_t Port() const noexcept;

    void Poll(std::vector<TransportEvent>& out) override;
    bool Send(TransportConnection connection, Channel channel, std::span<const uint8_t> data) override;
    /// The messages sent before it still go out first. The side that calls this sees no more events from
    /// that connection, and `Send` to it returns false.
    void Disconnect(TransportConnection connection) override;
    TransportStats Stats() const noexcept override;

    /// Hands the datagrams that `Send` queued to the socket now, instead of at the next `Poll`.
    void Flush();
    EnetWireStats Wire() noexcept;
    /// The `ENetHost*`, for tests and tuning only.
    void* NativeHost() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/// The client end: it asks for the connection when it is built, and sees the server as connection 0.
class EnetClientTransport final : public ITransport {
public:
    /// Throws std::runtime_error, with the cause, if the socket cannot be opened. A server that cannot be
    /// reached is not an error here: `Poll` reports `Disconnected` once the timeout has passed.
    explicit EnetClientTransport(const EnetClientConfig& config);
    ~EnetClientTransport() override;
    EnetClientTransport(const EnetClientTransport&) = delete;
    EnetClientTransport& operator=(const EnetClientTransport&) = delete;
    EnetClientTransport(EnetClientTransport&&) = delete;
    EnetClientTransport& operator=(EnetClientTransport&&) = delete;

    void Poll(std::vector<TransportEvent>& out) override;
    bool Send(TransportConnection connection, Channel channel, std::span<const uint8_t> data) override;
    void Disconnect(TransportConnection connection) override;
    TransportStats Stats() const noexcept override;

    void Flush();
    EnetWireStats Wire() noexcept;
    void* NativeHost() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tessera::net
