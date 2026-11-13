// SPDX-License-Identifier: Apache-2.0
#include "tessera_client.hpp"

#include "tessera/net/transport/enet_transport.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <memory>
#include <span>
#include <stdexcept>

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

bool TesseraClient::RefusedInRule() {
    if (!session_.RuleRunning()) {
        return false;
    }
    last_error_ = "a prediction rule cannot call back into the client";
    return true;
}

int TesseraClient::connect_to(const String& host, int port) {
    if (RefusedInRule()) {
        return ERR_BUSY;
    }
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
    if (RefusedInRule()) {
        return;
    }
    session_.Disconnect();
}

void TesseraClient::poll() {
    if (emitting_ || RefusedInRule()) {
        return;
    }
    // A handler may drop the last reference to this client; hold one until the signals are out.
    Ref<TesseraClient> keep_alive(this);
    try {
        // A correction of the prediction slides over real time: the time since the last poll, but not more
        // than a quarter of a second, so that a frame that took long does not make it jump.
        const auto now = std::chrono::steady_clock::now();
        if (has_last_poll_) {
            const double seconds = std::chrono::duration<double>(now - last_poll_).count();
            session_.Advance(seconds < 0.25 ? seconds : 0.25);
        }
        last_poll_ = now;
        has_last_poll_ = true;
        std::vector<ext::SessionEvent> events;
        session_.Poll(events);
        ReportRuleFailure();
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

int TesseraClient::set_prediction(int64_t entity, const PackedStringArray& properties, const Callable& rule) {
    if (RefusedInRule()) {
        return ERR_BUSY;
    }
    last_error_ = String();
    try {
        if (!rule.is_valid()) {
            last_error_ = "the rule is not a valid Callable";
            return ERR_INVALID_PARAMETER;
        }
        std::vector<std::string> names;
        for (int64_t i = 0; i < properties.size(); ++i) {
            names.emplace_back(properties[i].utf8().get_data());
        }
        // The rule of the game, as the session wants it: values in, values out. What a script can get wrong
        // is reported as an exception, which the session turns into the failure of the rule.
        const ext::ValueRule value_rule = [rule](std::vector<ext::NamedValue>& state,
                                                 std::span<const uint8_t> input, double dt_seconds) {
            Dictionary values;
            for (const ext::NamedValue& named : state) {
                values[ToGodot(named.name)] = ToVariant(named.value);
            }
            PackedByteArray bytes;
            bytes.resize(static_cast<int64_t>(input.size()));
            if (!input.empty()) {
                std::memcpy(bytes.ptrw(), input.data(), input.size());
            }
            const Variant result = rule.call(values, bytes, dt_seconds);
            if (result.get_type() != Variant::DICTIONARY) {
                throw std::runtime_error("the rule did not return a Dictionary");
            }
            const Dictionary changed = result;
            for (ext::NamedValue& named : state) {
                const String key = ToGodot(named.name);
                if (!changed.has(key)) {
                    continue; // a name that is left out keeps its value
                }
                const Variant value = changed[key];
                switch (named.value.kind) {
                case ext::Value::Kind::Vector3: {
                    if (value.get_type() != Variant::VECTOR3) {
                        throw std::runtime_error("the rule gave a value for '" + named.name +
                                                 "' that is not a Vector3");
                    }
                    const Vector3 vector = value;
                    named.value.vec[0] = vector.x;
                    named.value.vec[1] = vector.y;
                    named.value.vec[2] = vector.z;
                    break;
                }
                case ext::Value::Kind::Float:
                    if (value.get_type() != Variant::FLOAT) {
                        throw std::runtime_error("the rule gave a value for '" + named.name +
                                                 "' that is not a float");
                    }
                    named.value.number = value;
                    break;
                case ext::Value::Kind::Int:
                    if (value.get_type() != Variant::INT) {
                        throw std::runtime_error("the rule gave a value for '" + named.name +
                                                 "' that is not an int");
                    }
                    named.value.integer = value;
                    break;
                case ext::Value::Kind::Bool:
                    break; // not predicted
                }
            }
            return true;
        };
        ext::PredictionSettings settings;
        settings.smoothing_seconds = smoothing_seconds_;
        settings.snap_distance = snap_distance_;
        std::string error;
        if (!session_.SetPrediction(static_cast<uint64_t>(entity), names, value_rule, settings, error)) {
            last_error_ = ToGodot(error);
            return ERR_INVALID_PARAMETER;
        }
    } catch (const std::exception& exception) {
        last_error_ = ToGodot(std::string("could not set the prediction: ") + exception.what());
        return ERR_INVALID_PARAMETER;
    }
    return OK;
}

void TesseraClient::clear_prediction() {
    if (RefusedInRule()) {
        return;
    }
    session_.ClearPrediction();
}

Variant TesseraClient::get_predicted_property(int64_t entity, const String& name) const {
    try {
        const auto value =
            session_.PredictedProperty(static_cast<uint64_t>(entity), std::string(name.utf8().get_data()));
        return value ? ToVariant(*value) : Variant();
    } catch (const std::exception&) {
        return Variant();
    }
}

bool TesseraClient::is_predicting() const {
    return session_.Prediction().status == net::PredictionStatus::Active;
}

Dictionary TesseraClient::get_prediction_stats() const {
    Dictionary result;
    try {
        const ext::PredictionReport report = session_.Prediction();
        const char* state = "off";
        switch (report.status) {
        case net::PredictionStatus::Off:
            state = "off";
            break;
        case net::PredictionStatus::Waiting:
            state = "waiting";
            break;
        case net::PredictionStatus::Active:
            state = "active";
            break;
        case net::PredictionStatus::Paused:
            state = "paused";
            break;
        case net::PredictionStatus::Failed:
            state = "failed";
            break;
        }
        result["state"] = String(state);
        result["reconciliations"] = static_cast<int64_t>(report.stats.reconciliations);
        result["snaps"] = static_cast<int64_t>(report.stats.snaps);
        result["rule_failures"] = static_cast<int64_t>(report.stats.rule_failures);
        result["history_overflows"] = static_cast<int64_t>(report.stats.history_overflows);
        result["last_error"] = report.stats.last_error;
    } catch (const std::exception&) {
    }
    return result;
}

void TesseraClient::set_smoothing_seconds(double seconds) {
    if (RefusedInRule()) {
        return;
    }
    last_error_ = String();
    ext::PredictionSettings settings;
    settings.smoothing_seconds = seconds;
    settings.snap_distance = snap_distance_;
    std::string error;
    // The session checks the range when a prediction is set; without one it is checked here the same way.
    if (!(seconds >= 0.0 && seconds <= 5.0)) {
        last_error_ = "smoothing_seconds must be from 0 to 5";
        return;
    }
    const bool set = session_.Prediction().status != net::PredictionStatus::Off;
    if (set && !session_.SetPredictionSettings(settings, error)) {
        last_error_ = ToGodot(error);
        return;
    }
    smoothing_seconds_ = seconds;
}

double TesseraClient::get_smoothing_seconds() const {
    return smoothing_seconds_;
}

void TesseraClient::set_snap_distance(double distance) {
    if (RefusedInRule()) {
        return;
    }
    last_error_ = String();
    ext::PredictionSettings settings;
    settings.smoothing_seconds = smoothing_seconds_;
    settings.snap_distance = distance;
    std::string error;
    if (!(distance > 0.0 && std::isfinite(distance))) {
        last_error_ = "snap_distance must be above 0";
        return;
    }
    const bool set = session_.Prediction().status != net::PredictionStatus::Off;
    if (set && !session_.SetPredictionSettings(settings, error)) {
        last_error_ = ToGodot(error);
        return;
    }
    snap_distance_ = distance;
}

double TesseraClient::get_snap_distance() const {
    return snap_distance_;
}

// The first time the rule fails, one warning says why; the prediction is already off. The session hands the
// reason over only once for each failure.
void TesseraClient::ReportRuleFailure() {
    try {
        const std::string failure = session_.TakeRuleFailure();
        if (!failure.empty()) {
            UtilityFunctions::push_warning("TesseraClient: the prediction was turned off: " +
                                           ToGodot(failure));
        }
    } catch (const std::exception&) {
    }
}

int TesseraClient::send_input(const PackedByteArray& bytes) {
    if (RefusedInRule()) {
        return 0;
    }
    // The rule that runs inside this call is a script, which may drop the last reference to this client.
    Ref<TesseraClient> keep_alive(this);
    try {
        const std::span<const uint8_t> input(bytes.ptr(), static_cast<size_t>(bytes.size()));
        const int sequence = static_cast<int>(session_.SendInput(input));
        ReportRuleFailure();
        return sequence;
    } catch (...) {
        return 0;
    }
}

int TesseraClient::get_connection_id() const {
    return static_cast<int>(session_.ConnectionId());
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
    ClassDB::bind_method(D_METHOD("send_input", "bytes"), &TesseraClient::send_input);
    ClassDB::bind_method(D_METHOD("set_prediction", "entity", "properties", "rule"),
                         &TesseraClient::set_prediction);
    ClassDB::bind_method(D_METHOD("clear_prediction"), &TesseraClient::clear_prediction);
    ClassDB::bind_method(D_METHOD("get_predicted_property", "id", "name"),
                         &TesseraClient::get_predicted_property);
    ClassDB::bind_method(D_METHOD("is_predicting"), &TesseraClient::is_predicting);
    ClassDB::bind_method(D_METHOD("get_prediction_stats"), &TesseraClient::get_prediction_stats);
    ClassDB::bind_method(D_METHOD("set_smoothing_seconds", "seconds"), &TesseraClient::set_smoothing_seconds);
    ClassDB::bind_method(D_METHOD("get_smoothing_seconds"), &TesseraClient::get_smoothing_seconds);
    ClassDB::bind_method(D_METHOD("set_snap_distance", "distance"), &TesseraClient::set_snap_distance);
    ClassDB::bind_method(D_METHOD("get_snap_distance"), &TesseraClient::get_snap_distance);
    ClassDB::bind_method(D_METHOD("get_connection_id"), &TesseraClient::get_connection_id);
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
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "smoothing_seconds", PROPERTY_HINT_RANGE, "0,5,0.01,suffix:s"),
                 "set_smoothing_seconds", "get_smoothing_seconds");
    ADD_PROPERTY(
        PropertyInfo(Variant::FLOAT, "snap_distance", PROPERTY_HINT_RANGE, "0.01,100,0.01,or_greater"),
        "set_snap_distance", "get_snap_distance");

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
