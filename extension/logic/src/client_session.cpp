// SPDX-License-Identifier: Apache-2.0
#include "tessera/ext/client_session.hpp"

#include "tessera/net/entity_id.hpp"

#include <algorithm>
#include <exception>
#include <iterator>
#include <stdexcept>

namespace tessera::ext {

namespace {

SessionState ToSessionState(net::ClientState state) noexcept {
    switch (state) {
    case net::ClientState::Connecting:
        return SessionState::Connecting;
    case net::ClientState::Handshaking:
        return SessionState::Handshaking;
    case net::ClientState::Connected:
        return SessionState::Connected;
    case net::ClientState::Rejected:
        return SessionState::Rejected;
    case net::ClientState::Disconnected:
        return SessionState::Disconnected;
    }
    return SessionState::Disconnected;
}

bool WasConnecting(SessionState state) noexcept {
    return state == SessionState::Connecting || state == SessionState::Handshaking ||
           state == SessionState::Connected;
}

// Sets a flag for as long as it lives, and puts it back whatever happens.
class FlagGuard {
public:
    explicit FlagGuard(bool& flag) : flag_(flag) {
        flag_ = true;
    }
    ~FlagGuard() {
        flag_ = false;
    }
    FlagGuard(const FlagGuard&) = delete;
    FlagGuard& operator=(const FlagGuard&) = delete;

private:
    bool& flag_;
};

} // namespace

ClientSession::ClientSession(TransportFactory factory) : factory_(std::move(factory)) {
}

ClientSession::~ClientSession() {
    Close();
}

void ClientSession::Close() noexcept {
    try {
        if (replica_) {
            replica_->Disconnect(); // tells the server we are leaving
        }
    } catch (...) {
    }
    // The replica refers to the transport and to the schema, so they go in this order.
    replica_.reset();
    transport_.reset();
    schema_.reset();
    known_.clear();
    previous_ = SessionState::Disconnected;
}

bool ClientSession::Connect(std::shared_ptr<const BuiltSchema> schema, const std::string& host, int port,
                            std::string& error) {
    Disconnect();
    if (!schema) {
        error = "a schema is needed to connect";
        return false;
    }
    if (port < 1 || port > 65535) {
        error = "port " + std::to_string(port) + " is not valid (use 1 to 65535)";
        return false;
    }
    try {
        std::unique_ptr<net::ITransport> transport = factory_(host, static_cast<uint16_t>(port));
        if (!transport) {
            throw std::runtime_error("the transport did not open");
        }
        schema_ = std::move(schema);
        transport_ = std::move(transport);
        replica_ = std::make_unique<net::ClientReplica>(*transport_, schema_->Schema());
    } catch (const std::exception& exception) {
        Close();
        error = std::string("could not connect: ") + exception.what();
        return false;
    } catch (...) {
        Close();
        error = "could not connect";
        return false;
    }
    previous_ = SessionState::Connecting;
    ++generation_;
    return true;
}

void ClientSession::Disconnect() noexcept {
    const bool had_connection = replica_ != nullptr;
    Close();
    if (had_connection) {
        ++generation_;
    }
}

void ClientSession::Poll(std::vector<SessionEvent>& out) {
    if (!replica_ || polling_) {
        return;
    }
    const FlagGuard guard(polling_);
    replica_->Poll();
    const SessionState state = ToSessionState(replica_->State());

    std::vector<uint64_t> now;
    if (state == SessionState::Connected) {
        replica_->ForEachEntity([&now](net::EntityId id, const uint32_t*) { now.push_back(id.raw()); });
        std::sort(now.begin(), now.end());
    }

    if (state == SessionState::Connected && previous_ != SessionState::Connected) {
        out.push_back(SessionEvent{SessionEvent::Kind::Connected, 0, {}});
    }
    const bool dropped = state == SessionState::Disconnected && WasConnecting(previous_);
    if (dropped) {
        // The connection is gone, and with it everything the client knew.
        for (uint64_t entity : known_) {
            out.push_back(SessionEvent{SessionEvent::Kind::EntityRemoved, entity, {}});
        }
        now.clear();
    } else {
        std::vector<uint64_t> changed;
        std::set_difference(known_.begin(), known_.end(), now.begin(), now.end(), std::back_inserter(changed));
        for (uint64_t entity : changed) {
            out.push_back(SessionEvent{SessionEvent::Kind::EntityRemoved, entity, {}});
        }
        changed.clear();
        std::set_difference(now.begin(), now.end(), known_.begin(), known_.end(), std::back_inserter(changed));
        for (uint64_t entity : changed) {
            out.push_back(SessionEvent{SessionEvent::Kind::EntityAppeared, entity, {}});
        }
    }
    if (state == SessionState::Rejected && previous_ != SessionState::Rejected) {
        out.push_back(SessionEvent{SessionEvent::Kind::Rejected, 0, RejectionText()});
    }
    if (dropped) {
        out.push_back(SessionEvent{SessionEvent::Kind::Disconnected, 0, {}});
    }
    known_ = std::move(now);
    previous_ = state;
}

uint32_t ClientSession::SendInput(std::span<const uint8_t> bytes) noexcept {
    try {
        return replica_ ? replica_->SendInput(bytes) : 0; // 0 as well when the client is not connected
    } catch (...) {
        return 0;
    }
}

int64_t ClientSession::ConnectionId() const noexcept {
    if (!replica_ || replica_->State() != net::ClientState::Connected) {
        return -1;
    }
    return static_cast<int64_t>(replica_->ConnectionId());
}

SessionState ClientSession::State() const noexcept {
    return replica_ ? ToSessionState(replica_->State()) : SessionState::Disconnected;
}

std::string ClientSession::RejectionText() const {
    if (!replica_ || replica_->State() != net::ClientState::Rejected) {
        return {};
    }
    switch (replica_->Rejection()) {
    case net::RejectReason::SchemaMismatch:
        return "the server's schema is not the same as this client's: compare, on both sides, the types, the "
               "order of the properties, the kind of each one, and its settings (bits, step and range, min, max, "
               "signed, count)";
    case net::RejectReason::VersionMismatch:
        return "the server speaks a different version of the protocol";
    case net::RejectReason::ServerFull:
        return "the server is full";
    case net::RejectReason::None:
        break;
    }
    return "the server refused the connection";
}

bool ClientSession::IsKnown(uint64_t entity) const noexcept {
    return std::binary_search(known_.begin(), known_.end(), entity);
}

std::vector<uint64_t> ClientSession::Entities() const {
    return known_;
}

std::optional<Value> ClientSession::Property(uint64_t entity, std::string_view name) const {
    if (!replica_ || !IsKnown(entity)) {
        return std::nullopt;
    }
    const net::EntityId id = net::EntityId::FromRaw(entity);
    const uint32_t* words = replica_->WordsOf(id);
    const net::TypeSchema* type = schema_->Schema().Find(id.type());
    const std::vector<PropertyInfo>* infos = schema_->Properties(id.type());
    if (words == nullptr || type == nullptr || infos == nullptr) {
        return std::nullopt;
    }
    for (size_t i = 0; i < infos->size() && i < type->properties.size(); ++i) {
        if ((*infos)[i].name == name) {
            return ConvertProperty(type->properties[i], (*infos)[i].kind, words);
        }
    }
    return std::nullopt;
}

std::vector<EntityView> ClientSession::Snapshot() const {
    std::vector<EntityView> snapshot;
    if (!replica_) {
        return snapshot;
    }
    for (uint64_t entity : known_) {
        const net::EntityId id = net::EntityId::FromRaw(entity);
        const uint32_t* words = replica_->WordsOf(id);
        const net::TypeSchema* type = schema_->Schema().Find(id.type());
        const std::vector<PropertyInfo>* infos = schema_->Properties(id.type());
        if (words == nullptr || type == nullptr || infos == nullptr) {
            continue;
        }
        EntityView view;
        view.id = entity;
        view.type = id.type();
        for (size_t i = 0; i < infos->size() && i < type->properties.size(); ++i) {
            view.properties.push_back(
                NamedValue{(*infos)[i].name, ConvertProperty(type->properties[i], (*infos)[i].kind, words)});
        }
        snapshot.push_back(std::move(view));
    }
    return snapshot;
}

} // namespace tessera::ext
