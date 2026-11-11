// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/transport/enet_transport.hpp"

#include <enet/enet.h>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace tessera::net {

namespace {

// ENet keeps global state (on Windows: the socket library and the timer resolution), so it is started when
// the first transport is built and stopped when the last one is gone.
std::mutex g_enet_mutex;
int g_enet_users = 0;

void AcquireEnet() {
    std::lock_guard<std::mutex> lock(g_enet_mutex);
    if (g_enet_users == 0 && enet_initialize() != 0) {
        throw std::runtime_error("ENet could not be initialized");
    }
    ++g_enet_users;
}

void ReleaseEnet() noexcept {
    std::lock_guard<std::mutex> lock(g_enet_mutex);
    if (--g_enet_users == 0) {
        enet_deinitialize();
    }
}

struct EnetReference {
    EnetReference() {
        AcquireEnet();
    }
    ~EnetReference() {
        ReleaseEnet();
    }
    EnetReference(const EnetReference&) = delete;
    EnetReference& operator=(const EnetReference&) = delete;
};

constexpr enet_uint8 kReliableChannel = 0;
constexpr enet_uint8 kUnreliableChannel = 1;
constexpr size_t kChannelCount = 2;

struct HostDeleter {
    void operator()(ENetHost* host) const noexcept {
        if (host != nullptr) {
            enet_host_destroy(host);
        }
    }
};

// What the server and the client ends have in common: the ENet host, the message counters and the wire
// counters.
class Endpoint {
public:
    TransportStats Stats() const noexcept {
        return stats_;
    }

    void Flush() {
        enet_host_flush(host_.get());
        AbsorbWire();
    }

    EnetWireStats Wire() noexcept {
        AbsorbWire();
        return wire_;
    }

    void* NativeHost() noexcept {
        return host_.get();
    }

protected:
    Endpoint() = default;

    void SetHost(ENetHost* host) {
        host_.reset(host);
    }
    ENetHost* Host() const noexcept {
        return host_.get();
    }

    // ENet counts in 32 bits and wraps; the difference since the last look is exact if looked at often
    // enough.
    void AbsorbWire() noexcept {
        if (!host_) {
            return;
        }
        wire_.bytes_sent += static_cast<uint32_t>(host_->totalSentData - last_sent_);
        wire_.bytes_received += static_cast<uint32_t>(host_->totalReceivedData - last_received_);
        last_sent_ = host_->totalSentData;
        last_received_ = host_->totalReceivedData;
    }

    // Sends one message. On a failure the packet is destroyed here; on success ENet owns it.
    bool SendOn(ENetPeer* peer, Channel channel, std::span<const uint8_t> data) {
        if (peer == nullptr || peer->state != ENET_PEER_STATE_CONNECTED) {
            return false;
        }
        const bool reliable = channel == Channel::ReliableOrdered;
        // An unreliable message is sent as unreliable fragments: without that flag ENet splits a message
        // larger than the MTU into reliable fragments, and it would be sent again until it arrived.
        const enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
        ENetPacket* packet = enet_packet_create(data.data(), data.size(), flags);
        if (packet == nullptr) {
            return false;
        }
        if (enet_peer_send(peer, reliable ? kReliableChannel : kUnreliableChannel, packet) < 0) {
            enet_packet_destroy(packet);
            return false;
        }
        ++stats_.messages_sent;
        stats_.bytes_sent += data.size();
        return true;
    }

    // Moves a received packet into `out` (when `deliver`) and destroys it in every case.
    void TakePacket(ENetEvent& event, TransportConnection connection, bool deliver,
                    std::vector<TransportEvent>& out) {
        ENetPacket* packet = event.packet;
        if (deliver && (event.channelID == kReliableChannel || event.channelID == kUnreliableChannel)) {
            TransportEvent message{TransportEvent::Kind::Message, connection,
                                   event.channelID == kReliableChannel ? Channel::ReliableOrdered
                                                                       : Channel::Unreliable,
                                   std::vector<uint8_t>(packet->data, packet->data + packet->dataLength)};
            ++stats_.messages_received;
            stats_.bytes_received += message.data.size();
            out.push_back(std::move(message));
        }
        enet_packet_destroy(packet);
    }

    // Disconnects every peer at once (used when the transport is destroyed).
    void DropAllPeers() noexcept {
        if (!host_) {
            return;
        }
        for (size_t i = 0; i < host_->peerCount; ++i) {
            ENetPeer* peer = &host_->peers[i];
            if (peer->state != ENET_PEER_STATE_DISCONNECTED) {
                enet_peer_disconnect_now(peer, 0);
            }
        }
        enet_host_flush(host_.get());
    }

    EnetReference enet_; // first member: ENet outlives the host
    std::unique_ptr<ENetHost, HostDeleter> host_;
    TransportStats stats_;
    EnetWireStats wire_;
    enet_uint32 last_sent_ = 0;
    enet_uint32 last_received_ = 0;
};

ENetAddress MakeAddress(const std::string& host, uint16_t port, bool any_if_empty) {
    ENetAddress address{};
    if (host.empty() && any_if_empty) {
        address.host = ENET_HOST_ANY;
    } else if (enet_address_set_host_ip(&address, host.c_str()) != 0 &&
               enet_address_set_host(&address, host.c_str()) != 0) {
        throw std::runtime_error("ENet could not resolve the address '" + host + "'");
    }
    address.port = port;
    return address;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------
// Server

class EnetServerTransport::Impl final : public Endpoint {
public:
    explicit Impl(const EnetServerConfig& config) {
        if (config.max_clients < 1 || config.max_clients > ENET_PROTOCOL_MAXIMUM_PEER_ID) {
            throw std::runtime_error("max_clients must be from 1 to " +
                                     std::to_string(ENET_PROTOCOL_MAXIMUM_PEER_ID));
        }
        const ENetAddress address = MakeAddress(config.bind_address, config.port, true);
        ENetHost* host = enet_host_create(&address, config.max_clients, kChannelCount, 0, 0);
        if (host == nullptr) {
            throw std::runtime_error("ENet could not open a server socket on port " +
                                     std::to_string(config.port) + " (is it in use?)");
        }
        SetHost(host);
        ENetAddress bound{};
        port_ = enet_socket_get_address(host->socket, &bound) == 0 ? bound.port : config.port;
    }

    ~Impl() {
        DropAllPeers();
    }

    uint16_t Port() const noexcept {
        return port_;
    }

    void Poll(std::vector<TransportEvent>& out) {
        ENetEvent event;
        while (enet_host_service(Host(), &event, 0) > 0) {
            switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                const TransportConnection id = next_id_++; // never used again, whatever slot ENet picked
                event.peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(id));
                connections_[id] = Connection{event.peer, false};
                out.push_back(TransportEvent{TransportEvent::Kind::Connected, id});
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT: {
                const TransportConnection id = IdOf(event.peer);
                event.peer->data = nullptr;
                const auto it = connections_.find(id);
                if (it != connections_.end()) {
                    // The side that closed a connection sees nothing more of it.
                    const bool closed_here = it->second.closed;
                    connections_.erase(it);
                    if (!closed_here) {
                        out.push_back(TransportEvent{TransportEvent::Kind::Disconnected, id});
                    }
                }
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                const TransportConnection id = IdOf(event.peer);
                const auto it = connections_.find(id);
                TakePacket(event, id, it != connections_.end() && !it->second.closed, out);
                break;
            }
            default:
                break;
            }
        }
        AbsorbWire();
    }

    bool Send(TransportConnection connection, Channel channel, std::span<const uint8_t> data) {
        const auto it = connections_.find(connection);
        if (it == connections_.end() || it->second.closed) {
            return false;
        }
        return SendOn(it->second.peer, channel, data);
    }

    void Disconnect(TransportConnection connection) {
        const auto it = connections_.find(connection);
        if (it == connections_.end() || it->second.closed) {
            return;
        }
        it->second.closed = true;
        enet_peer_disconnect_later(it->second.peer, 0); // what was sent before it still goes out first
    }

private:
    struct Connection {
        ENetPeer* peer = nullptr;
        bool closed = false; // closed by this side: nothing more is delivered or accepted
    };

    static TransportConnection IdOf(const ENetPeer* peer) noexcept {
        return static_cast<TransportConnection>(reinterpret_cast<uintptr_t>(peer->data));
    }

    std::unordered_map<TransportConnection, Connection> connections_;
    TransportConnection next_id_ = 1;
    uint16_t port_ = 0;
};

EnetServerTransport::EnetServerTransport(const EnetServerConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}
EnetServerTransport::~EnetServerTransport() = default;

uint16_t EnetServerTransport::Port() const noexcept {
    return impl_->Port();
}
void EnetServerTransport::Poll(std::vector<TransportEvent>& out) {
    impl_->Poll(out);
}
bool EnetServerTransport::Send(TransportConnection connection, Channel channel,
                               std::span<const uint8_t> data) {
    return impl_->Send(connection, channel, data);
}
void EnetServerTransport::Disconnect(TransportConnection connection) {
    impl_->Disconnect(connection);
}
TransportStats EnetServerTransport::Stats() const noexcept {
    return impl_->Stats();
}
void EnetServerTransport::Flush() {
    impl_->Flush();
}
EnetWireStats EnetServerTransport::Wire() noexcept {
    return impl_->Wire();
}
void* EnetServerTransport::NativeHost() noexcept {
    return impl_->NativeHost();
}

// ---------------------------------------------------------------------------------------------------------
// Client

class EnetClientTransport::Impl final : public Endpoint {
public:
    explicit Impl(const EnetClientConfig& config) {
        const ENetAddress address = MakeAddress(config.server_address, config.server_port, false);
        ENetHost* host = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
        if (host == nullptr) {
            throw std::runtime_error("ENet could not open a client socket");
        }
        SetHost(host);
        peer_ = enet_host_connect(host, &address, kChannelCount, 0);
        if (peer_ == nullptr) {
            throw std::runtime_error("ENet could not start a connection to " + config.server_address);
        }
        // The same limit while connecting and once connected.
        enet_peer_timeout(peer_, ENET_PEER_TIMEOUT_LIMIT, config.timeout_ms, config.timeout_ms);
    }

    ~Impl() {
        DropAllPeers();
    }

    void Poll(std::vector<TransportEvent>& out) {
        ENetEvent event;
        while (enet_host_service(Host(), &event, 0) > 0) {
            switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                if (!closed_) {
                    out.push_back(TransportEvent{TransportEvent::Kind::Connected, 0});
                }
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                // Also what a connection that never opened turns into once the timeout has passed.
                if (!closed_) {
                    out.push_back(TransportEvent{TransportEvent::Kind::Disconnected, 0});
                }
                ended_ = true;
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                TakePacket(event, 0, !closed_, out);
                break;
            default:
                break;
            }
        }
        AbsorbWire();
    }

    bool Send(TransportConnection connection, Channel channel, std::span<const uint8_t> data) {
        if (connection != 0 || closed_ || ended_) {
            return false;
        }
        return SendOn(peer_, channel, data);
    }

    void Disconnect(TransportConnection connection) {
        if (connection != 0 || closed_ || ended_) {
            return;
        }
        closed_ = true;
        enet_peer_disconnect_later(peer_, 0);
    }

private:
    ENetPeer* peer_ = nullptr;
    bool closed_ = false; // closed by this side
    bool ended_ = false;  // ENet reported the end of the connection
};

EnetClientTransport::EnetClientTransport(const EnetClientConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}
EnetClientTransport::~EnetClientTransport() = default;

void EnetClientTransport::Poll(std::vector<TransportEvent>& out) {
    impl_->Poll(out);
}
bool EnetClientTransport::Send(TransportConnection connection, Channel channel,
                               std::span<const uint8_t> data) {
    return impl_->Send(connection, channel, data);
}
void EnetClientTransport::Disconnect(TransportConnection connection) {
    impl_->Disconnect(connection);
}
TransportStats EnetClientTransport::Stats() const noexcept {
    return impl_->Stats();
}
void EnetClientTransport::Flush() {
    impl_->Flush();
}
EnetWireStats EnetClientTransport::Wire() noexcept {
    return impl_->Wire();
}
void* EnetClientTransport::NativeHost() noexcept {
    return impl_->NativeHost();
}

} // namespace tessera::net
