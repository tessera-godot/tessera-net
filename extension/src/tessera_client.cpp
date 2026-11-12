// SPDX-License-Identifier: Apache-2.0
#include "tessera_client.hpp"

#include "tessera/net/transport/enet_transport.hpp"

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <exception>
#include <memory>

namespace godot {

namespace {

namespace ext = tessera::ext;
namespace net = tessera::net;

String ToGodot(const std::string& text) {
    return String::utf8(text.c_str(), static_cast<int64_t>(text.size()));
}

// The ENet client, flushed after everything that asks it to send: the session has no flush of its own to call.
class FlushingEnetClient final : public net::ITransport {
public:
    explicit FlushingEnetClient(const net::EnetClientConfig& config) : inner_(config) {
    }

    void Poll(std::vector<net::TransportEvent>& out) override {
        inner_.Poll(out);
        inner_.Flush();
    }
    bool Send(net::TransportConnection connection, net::Channel channel, std::span<const uint8_t> data) override {
        const bool sent = inner_.Send(connection, channel, data);
        inner_.Flush();
        return sent;
    }
    void Disconnect(net::TransportConnection connection) override {
        inner_.Disconnect(connection);
        inner_.Flush();
    }
    net::TransportStats Stats() const noexcept override {
        return inner_.Stats();
    }

private:
    net::EnetClientTransport inner_;
};

std::unique_ptr<net::ITransport> OpenEnet(const std::string& host, uint16_t port) {
    net::EnetClientConfig config;
    config.server_address = host;
    config.server_port = port;
    return std::make_unique<FlushingEnetClient>(config);
}

Variant ToVariant(const ext::Value& value) {
    switch (value.kind) {
    case ext::Value::Kind::Vector3:
        return Vector3(static_cast<real_t>(value.vec[0]), static_cast<real_t>(value.vec[1]),
                       static_cast<real_t>(value.vec[2]));
    case ext::Value::Kind::Float:
        return value.number;
    case ext::Value::Kind::Int:
        return value.integer;
    case ext::Value::Kind::Bool:
        return value.flag;
    }
    return Variant();
}

} // namespace

TesseraClient::TesseraClient() : session_(OpenEnet) {
}

TesseraClient::~TesseraClient() {
    // The session says goodbye to the server when it goes; nothing may escape from a destructor.
    try {
        session_.Disconnect();
    } catch (...) {
    }
}

int TesseraClient::connect_to(const String& host, int port) {
    last_error_ = String();
    std::string error;
    try {
        std::shared_ptr<const ext::BuiltSchema> built;
        if (schema_.is_valid()) {
            built = schema_->Build(error);
        } else {
            error = "set a schema before connecting";
        }
        if (built == nullptr) {
            session_.Disconnect();
            last_error_ = ToGodot(error);
            return ERR_INVALID_PARAMETER;
        }
        if (!session_.Connect(built, std::string(host.utf8().get_data()), port, error)) {
            last_error_ = ToGodot(error);
            return ERR_INVALID_PARAMETER;
        }
    } catch (const std::exception& exception) {
        last_error_ = ToGodot(std::string("could not connect: ") + exception.what());
        return ERR_INVALID_PARAMETER;
    }
    return OK;
}

void TesseraClient::disconnect_from_server() {
    session_.Disconnect();
}

void TesseraClient::poll() {
    if (emitting_) {
        return;
    }
    // A handler may drop the last reference to this client; hold one until the signals are out.
    Ref<TesseraClient> keep_alive(this);
    try {
        std::vector<ext::SessionEvent> events;
        session_.Poll(events);
        const uint64_t generation = session_.Generation();
        emitting_ = true;
        for (const ext::SessionEvent& event : events) {
            // A handler that connected again or disconnected has changed what these events are about.
            if (session_.Generation() != generation) {
                break;
            }
            switch (event.kind) {
            case ext::SessionEvent::Kind::Connected:
                emit_signal("connected");
                break;
            case ext::SessionEvent::Kind::Disconnected:
                emit_signal("disconnected");
                break;
            case ext::SessionEvent::Kind::Rejected:
                emit_signal("rejected", ToGodot(event.reason));
                break;
            case ext::SessionEvent::Kind::EntityAppeared:
                emit_signal("entity_appeared", static_cast<int64_t>(event.entity));
                break;
            case ext::SessionEvent::Kind::EntityRemoved:
                emit_signal("entity_removed", static_cast<int64_t>(event.entity));
                break;
            }
        }
    } catch (const std::exception& exception) {
        last_error_ = ToGodot(std::string("poll failed: ") + exception.what());
    }
    emitting_ = false;
}

int TesseraClient::get_state() const {
    return static_cast<int>(session_.State());
}

String TesseraClient::get_rejection() const {
    try {
        return ToGodot(session_.RejectionText());
    } catch (const std::exception&) {
        return String();
    }
}

String TesseraClient::get_last_error() const {
    return last_error_;
}

PackedInt64Array TesseraClient::get_entities() const {
    PackedInt64Array result;
    try {
        for (uint64_t entity : session_.Entities()) {
            result.push_back(static_cast<int64_t>(entity));
        }
    } catch (const std::exception&) {
    }
    return result;
}

Variant TesseraClient::get_property(int64_t entity, const String& name) const {
    try {
        const auto value = session_.Property(static_cast<uint64_t>(entity), std::string(name.utf8().get_data()));
        return value ? ToVariant(*value) : Variant();
    } catch (const std::exception&) {
        return Variant();
    }
}

Dictionary TesseraClient::get_snapshot() const {
    Dictionary result;
    try {
        for (const ext::EntityView& entity : session_.Snapshot()) {
            Dictionary values;
            for (const ext::NamedValue& property : entity.properties) {
                values[ToGodot(property.name)] = ToVariant(property.value);
            }
            result[static_cast<int64_t>(entity.id)] = values;
        }
    } catch (const std::exception&) {
    }
    return result;
}

void TesseraClient::set_schema(const Ref<TesseraSchema>& schema) {
    schema_ = schema;
}

Ref<TesseraSchema> TesseraClient::get_schema() const {
    return schema_;
}

void TesseraClient::_bind_methods() {
    ClassDB::bind_method(D_METHOD("connect_to", "host", "port"), &TesseraClient::connect_to);
    ClassDB::bind_method(D_METHOD("disconnect_from_server"), &TesseraClient::disconnect_from_server);
    ClassDB::bind_method(D_METHOD("poll"), &TesseraClient::poll);
    ClassDB::bind_method(D_METHOD("get_state"), &TesseraClient::get_state);
    ClassDB::bind_method(D_METHOD("get_rejection"), &TesseraClient::get_rejection);
    ClassDB::bind_method(D_METHOD("get_last_error"), &TesseraClient::get_last_error);
    ClassDB::bind_method(D_METHOD("get_entities"), &TesseraClient::get_entities);
    ClassDB::bind_method(D_METHOD("get_property", "id", "name"), &TesseraClient::get_property);
    ClassDB::bind_method(D_METHOD("get_snapshot"), &TesseraClient::get_snapshot);
    ClassDB::bind_method(D_METHOD("set_schema", "schema"), &TesseraClient::set_schema);
    ClassDB::bind_method(D_METHOD("get_schema"), &TesseraClient::get_schema);
    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "schema", PROPERTY_HINT_RESOURCE_TYPE, "TesseraSchema"),
                 "set_schema", "get_schema");

    BIND_ENUM_CONSTANT(CONNECTING);
    BIND_ENUM_CONSTANT(HANDSHAKING);
    BIND_ENUM_CONSTANT(CONNECTED);
    BIND_ENUM_CONSTANT(REJECTED);
    BIND_ENUM_CONSTANT(DISCONNECTED);

    ADD_SIGNAL(MethodInfo("connected"));
    ADD_SIGNAL(MethodInfo("disconnected"));
    ADD_SIGNAL(MethodInfo("rejected", PropertyInfo(Variant::STRING, "reason")));
    ADD_SIGNAL(MethodInfo("entity_appeared", PropertyInfo(Variant::INT, "id")));
    ADD_SIGNAL(MethodInfo("entity_removed", PropertyInfo(Variant::INT, "id")));
}

} // namespace godot
