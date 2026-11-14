// SPDX-License-Identifier: Apache-2.0
#include "tessera/ext/client_session.hpp"

#include "tessera/net/entity_id.hpp"

#include <algorithm>
#include <cmath>
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

const char* const kRefusedInRule = "a prediction rule cannot call back into the client";

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
    prediction_.reset();
    predicted_numbers_.clear();
    transport_.reset();
    schema_.reset();
    known_.clear();
    previous_ = SessionState::Disconnected;
}

bool ClientSession::Connect(std::shared_ptr<const BuiltSchema> schema, const std::string& host, int port,
                            std::string& error) {
    if (RuleRunning()) {
        error = kRefusedInRule;
        return false;
    }
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
        if (interpolation_set_) {
            replica_->SetInterpolation(interpolation_, nullptr); // the settings outlast the connection
        }
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
    if (RuleRunning()) {
        return; // it would destroy the connection the rule is running on
    }
    const bool had_connection = replica_ != nullptr;
    Close();
    if (had_connection) {
        ++generation_;
    }
}

void ClientSession::Poll(std::vector<SessionEvent>& out) {
    if (!replica_ || polling_ || RuleRunning()) {
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
        if (RuleRunning()) {
            return 0;
        }
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

// What a prediction needs besides the core's: the rule as the game gave it, the names it works on (in the
// order of the numbers the core hands over), and why it failed, if it did.
struct ClientSession::PredictionState {
    struct Slot {
        ValueKind kind;
        uint32_t offset; // of the first number in the core's state
    };

    ValueRule rule;
    uint64_t entity = 0;
    std::vector<Slot> slots;
    std::vector<NamedValue> named; // one for each slot, reused by every call of the rule
    std::string failure;
    bool reported = false; // `TakeRuleFailure` has handed the failure over

    void Note(const std::string& reason) {
        if (failure.empty()) {
            failure = reason;
        }
    }

    // The core's numbers as the value of one slot.
    Value ValueOf(size_t index, std::span<const double> numbers) const {
        const Slot& slot = slots[index];
        Value value;
        switch (slot.kind) {
        case ValueKind::Position:
            value.kind = Value::Kind::Vector3;
            for (uint32_t axis = 0; axis < 3; ++axis) {
                value.vec[axis] = numbers[slot.offset + axis];
            }
            break;
        case ValueKind::Angle:
            value.kind = Value::Kind::Float;
            value.number = numbers[slot.offset];
            break;
        default:
            value.kind = Value::Kind::Int;
            value.integer = static_cast<int64_t>(std::llround(numbers[slot.offset]));
            break;
        }
        return value;
    }

    // What the rule did to the values, back as the core's numbers. False if a value is not of its kind.
    bool Store(std::span<double> numbers) {
        if (named.size() != slots.size()) {
            Note("the rule changed the list of predicted properties");
            return false;
        }
        for (size_t i = 0; i < slots.size(); ++i) {
            const Slot& slot = slots[i];
            const Value& value = named[i].value;
            const char* wanted = nullptr;
            switch (slot.kind) {
            case ValueKind::Position:
                if (value.kind == Value::Kind::Vector3) {
                    for (uint32_t axis = 0; axis < 3; ++axis) {
                        numbers[slot.offset + axis] = value.vec[axis];
                    }
                } else {
                    wanted = "a Vector3";
                }
                break;
            case ValueKind::Angle:
                if (value.kind == Value::Kind::Float) {
                    numbers[slot.offset] = value.number;
                } else {
                    wanted = "a float";
                }
                break;
            default:
                if (value.kind == Value::Kind::Int) {
                    numbers[slot.offset] = static_cast<double>(value.integer);
                } else {
                    wanted = "an int";
                }
                break;
            }
            if (wanted != nullptr) {
                Note("the rule gave a value of the wrong type for '" + named[i].name + "' (" + wanted +
                     " was needed)");
                return false;
            }
        }
        return true;
    }

    bool Run(std::span<double> numbers, std::span<const uint8_t> input, double dt_seconds) {
        try {
            for (size_t i = 0; i < slots.size(); ++i) {
                named[i].value = ValueOf(i, numbers);
            }
            if (!rule(named, input, dt_seconds)) {
                Note("the rule returned false");
                return false;
            }
            return Store(numbers);
        } catch (const std::exception& exception) {
            Note(std::string("the rule threw an exception: ") + exception.what());
            return false;
        } catch (...) {
            Note("the rule threw an exception");
            return false;
        }
    }
};

bool ClientSession::SetPrediction(uint64_t entity, const std::vector<std::string>& properties, ValueRule rule,
                                  const PredictionSettings& settings, std::string& error) {
    try {
        if (RuleRunning()) {
            error = kRefusedInRule;
            return false;
        }
        if (!replica_) {
            error = "there is no connection to predict on";
            return false;
        }
        const net::EntityId id = net::EntityId::FromRaw(entity);
        if (entity == 0 || id.IsNull()) {
            error = "the entity to predict is not valid";
            return false;
        }
        const std::vector<PropertyInfo>* infos = schema_->Properties(id.type());
        if (infos == nullptr) {
            error = "the type of that entity is not in the schema";
            return false;
        }
        if (properties.empty()) {
            error = "name at least one property to predict";
            return false;
        }
        if (!rule) {
            error = "the prediction needs a rule";
            return false;
        }
        auto state = std::make_shared<PredictionState>();
        state->rule = std::move(rule);
        state->entity = entity;
        net::PredictionConfig config;
        config.entity = id;
        config.smoothing_seconds = settings.smoothing_seconds;
        config.snap_distance = settings.snap_distance;
        uint32_t offset = 0;
        for (const std::string& name : properties) {
            size_t index = 0;
            while (index < infos->size() && (*infos)[index].name != name) {
                ++index;
            }
            if (index == infos->size()) {
                error = "the type has no property called '" + name + "'";
                return false;
            }
            const ValueKind kind = (*infos)[index].kind;
            if (kind == ValueKind::Flag || kind == ValueKind::Mask) {
                error = "property '" + name + "' is a flag or a mask and cannot be predicted";
                return false;
            }
            config.properties.push_back(static_cast<uint32_t>(index));
            state->slots.push_back({kind, offset});
            state->named.push_back(NamedValue{name, Value{}});
            offset += kind == ValueKind::Position ? 3u : 1u;
        }
        config.rule = [state](std::span<double> numbers, std::span<const uint8_t> input, double dt_seconds) {
            return state->Run(numbers, input, dt_seconds);
        };
        if (!replica_->SetPrediction(config, &error)) {
            return false;
        }
        prediction_ = std::move(state);
        predicted_numbers_.assign(replica_->PredictedComponents(), 0.0); // so that reading does not allocate
        return true;
    } catch (const std::exception& exception) {
        error = std::string("could not set the prediction: ") + exception.what();
    } catch (...) {
        error = "could not set the prediction";
    }
    return false;
}

void ClientSession::ClearPrediction() noexcept {
    if (RuleRunning()) {
        return;
    }
    if (replica_) {
        replica_->ClearPrediction();
    }
    prediction_.reset();
    predicted_numbers_.clear();
}

bool ClientSession::SetPredictionSettings(const PredictionSettings& settings, std::string& error) {
    try {
        if (RuleRunning()) {
            error = kRefusedInRule;
            return false;
        }
        if (!replica_) {
            error = "nothing is being predicted";
            return false;
        }
        return replica_->SetPredictionSettings(settings.smoothing_seconds, settings.snap_distance, &error);
    } catch (...) {
        error = "could not change the settings";
        return false;
    }
}

void ClientSession::Advance(double seconds) noexcept {
    if (replica_ && !RuleRunning()) {
        replica_->AdvancePrediction(seconds);
        replica_->AdvanceInterpolation(seconds);
    }
}

std::optional<Value> ClientSession::PredictedProperty(uint64_t entity, std::string_view name) const {
    if (replica_ && prediction_ && entity == prediction_->entity &&
        replica_->Prediction() == net::PredictionStatus::Active) {
        for (size_t i = 0; i < prediction_->slots.size(); ++i) {
            if (prediction_->named[i].name != name) {
                continue;
            }
            if (predicted_numbers_.size() == replica_->PredictedComponents() &&
                replica_->Predicted(predicted_numbers_)) {
                return prediction_->ValueOf(i, predicted_numbers_);
            }
        }
    }
    return Property(entity, name);
}

bool ClientSession::SetInterpolation(uint32_t delay_ticks, double snap_distance, std::string& error) {
    try {
        if (delay_ticks > net::Interpolator::kMaxDelayTicks) {
            error = "delay_ticks must be from 0 to " + std::to_string(net::Interpolator::kMaxDelayTicks);
            return false;
        }
        if (!std::isfinite(snap_distance) || snap_distance <= 0.0) {
            error = "snap_distance must be a number above 0";
            return false;
        }
        net::InterpolationConfig config;
        config.delay_ticks = delay_ticks;
        config.snap_distance = snap_distance;
        if (replica_ && !replica_->SetInterpolation(config, &error)) {
            return false;
        }
        interpolation_ = config;
        interpolation_set_ = true;
        return true;
    } catch (const std::exception& exception) {
        error = std::string("could not set the interpolation: ") + exception.what();
    } catch (...) {
        error = "could not set the interpolation";
    }
    return false;
}

void ClientSession::ClearInterpolation() noexcept {
    interpolation_set_ = false;
    if (replica_) {
        replica_->ClearInterpolation();
    }
}

std::optional<Value> ClientSession::InterpolatedProperty(uint64_t entity, std::string_view name) const {
    if (!replica_ || !IsKnown(entity)) {
        return std::nullopt;
    }
    // The entity of the player is shown as the prediction has it: a copy a couple of ticks in the past would
    // lag behind the one the player is driving.
    if (prediction_ && entity == prediction_->entity &&
        replica_->Prediction() != net::PredictionStatus::Off) {
        return PredictedProperty(entity, name);
    }
    if (!replica_->Interpolating()) {
        return Property(entity, name);
    }
    const net::EntityId id = net::EntityId::FromRaw(entity);
    const net::TypeSchema* type = schema_->Schema().Find(id.type());
    const std::vector<PropertyInfo>* infos = schema_->Properties(id.type());
    if (type == nullptr || infos == nullptr) {
        return std::nullopt;
    }
    for (size_t i = 0; i < infos->size() && i < type->properties.size(); ++i) {
        if ((*infos)[i].name != name) {
            continue;
        }
        const ValueKind kind = (*infos)[i].kind;
        const uint32_t index = static_cast<uint32_t>(i);
        double numbers[3] = {0.0, 0.0, 0.0};
        Value value;
        if (kind == ValueKind::Position && replica_->DisplayedValue(id, index, numbers)) {
            value.kind = Value::Kind::Vector3;
            for (int axis = 0; axis < 3; ++axis) {
                value.vec[axis] = numbers[axis];
            }
            return value;
        }
        if (kind == ValueKind::Angle && replica_->DisplayedValue(id, index, numbers)) {
            value.kind = Value::Kind::Float;
            value.number = numbers[0];
            return value;
        }
        if (kind != ValueKind::Position && kind != ValueKind::Angle) {
            // An integer, a flag, a mask or a choice does not take in-between values: it is the state in
            // force at the time shown.
            if (const uint32_t* words = replica_->DisplayedWords(id)) {
                return ConvertProperty(type->properties[i], kind, words);
            }
        }
        break;
    }
    return Property(entity, name);
}

InterpolationReport ClientSession::Interpolation() const {
    InterpolationReport report;
    report.configured = interpolation_set_;
    report.delay_ticks = interpolation_.delay_ticks;
    report.snap_distance = interpolation_.snap_distance;
    if (replica_ && interpolation_set_) {
        report.running = replica_->Interpolating();
        report.stats = replica_->InterpolationInfo();
        report.lag_ticks = replica_->InterpolationLag();
    }
    return report;
}

bool ClientSession::RuleRunning() const noexcept {
    return replica_ != nullptr && replica_->RuleRunning();
}

std::string ClientSession::TakeRuleFailure() {
    if (replica_ && prediction_ && !prediction_->reported &&
        replica_->Prediction() == net::PredictionStatus::Failed) {
        prediction_->reported = true;
        // The rule can also fail by leaving a number that is not finite, which has no text of its own.
        return prediction_->failure.empty() ? std::string("the rule left a number that is not finite")
                                            : prediction_->failure;
    }
    return {};
}

PredictionReport ClientSession::Prediction() const {
    PredictionReport report;
    if (replica_) {
        report.status = replica_->Prediction();
        report.stats = replica_->PredictionInfo();
        if (prediction_) {
            report.failure = prediction_->failure;
        }
    }
    return report;
}

} // namespace tessera::ext
