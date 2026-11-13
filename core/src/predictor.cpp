// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/predictor.hpp"

#include "tessera/net/replication/property_codecs.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace tessera::net {

namespace {

constexpr double kMaxSmoothingSeconds = 5.0;

void Describe(std::string* error, const char* text) {
    if (error != nullptr) {
        *error = text;
    }
}

uint32_t ComponentsOf(PropertyKind kind) noexcept {
    return kind == PropertyKind::Position ? 3u : 1u;
}

} // namespace

Predictor::Predictor(const ReplicationSchema& schema) : schema_(schema) {
}

bool Predictor::Configure(const PredictionConfig& config, std::string* error) {
    if (config.entity.IsNull()) {
        Describe(error, "the entity to predict is not valid");
        return false;
    }
    const TypeSchema* type = schema_.Find(config.entity.type());
    if (type == nullptr) {
        Describe(error, "the type of the entity has no replication schema");
        return false;
    }
    if (config.properties.empty()) {
        Describe(error, "at least one property must be predicted");
        return false;
    }
    if (!config.rule) {
        Describe(error, "the prediction needs a rule");
        return false;
    }
    if (!std::isfinite(config.smoothing_seconds) || config.smoothing_seconds < 0.0 ||
        config.smoothing_seconds > kMaxSmoothingSeconds) {
        Describe(error, "the smoothing time must be from 0 to 5 seconds");
        return false;
    }
    if (!std::isfinite(config.snap_distance) || config.snap_distance <= 0.0) {
        Describe(error, "the snap distance must be a number above 0");
        return false;
    }
    std::vector<Slot> slots;
    slots.reserve(config.properties.size());
    std::vector<bool> used(type->properties.size(), false);
    uint32_t components = 0;
    for (const uint32_t index : config.properties) {
        if (index >= type->properties.size()) {
            Describe(error, "a predicted property is not one of the type's properties");
            return false;
        }
        if (used[index]) {
            Describe(error, "a property can be predicted only once");
            return false;
        }
        used[index] = true;
        const PropertyDescriptor& property = type->properties[index];
        if (property.kind == PropertyKind::Bits) {
            Describe(error, "a bit mask cannot be predicted");
            return false;
        }
        slots.push_back(Slot{property.kind, &property, components});
        components += ComponentsOf(property.kind);
    }
    // Everything the prediction needs later is made here, so that it does not allocate while the game runs.
    config_ = config;
    slots_ = std::move(slots);
    components_ = components;
    stats_ = PredictionStats{};
    state_.assign(components_, 0.0);
    work_.assign(components_, 0.0);
    ClearState();
    status_ = PredictionStatus::Waiting;
    return true;
}

void Predictor::Clear() noexcept {
    config_ = PredictionConfig{};
    slots_.clear();
    components_ = 0;
    stats_ = PredictionStats{};
    state_.clear();
    work_.clear();
    ClearState();
    status_ = PredictionStatus::Off;
}

void Predictor::Reset() noexcept {
    ClearState();
    if (status_ != PredictionStatus::Off && status_ != PredictionStatus::Failed) {
        status_ = PredictionStatus::Waiting;
    }
}

void Predictor::ClearState() noexcept {
    head_ = 0;
    pending_ = 0;
    any_tick_ = false;
    last_tick_ = 0;
    for (double& value : state_) {
        value = 0.0;
    }
}

void Predictor::Fail() noexcept {
    status_ = PredictionStatus::Failed;
    ++stats_.rule_failures;
    head_ = 0;
    pending_ = 0;
}

bool Predictor::ApplyRule(std::span<double> state, std::span<const uint8_t> input,
                          double dt_seconds) noexcept {
    try {
        if (!config_.rule(state, input, dt_seconds)) {
            return false;
        }
    } catch (...) {
        return false; // whatever the game's rule throws stays here
    }
    for (const double value : state) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return true;
}

void Predictor::ReadState(const uint32_t* entity_words, std::span<double> out) const noexcept {
    for (const Slot& slot : slots_) {
        const PropertyDescriptor& property = *slot.descriptor;
        const uint32_t* words = entity_words + property.word_offset;
        double* at = out.data() + slot.offset;
        switch (slot.kind) {
        case PropertyKind::Position:
            DequantizePosition(property, words, at);
            break;
        case PropertyKind::Angle:
            *at = DequantizeAngle(property, words);
            break;
        case PropertyKind::Integer:
            *at = static_cast<double>(IntegerValue(property, words));
            break;
        case PropertyKind::Enum:
            *at = static_cast<double>(words[0]);
            break;
        case PropertyKind::Bits:
            break; // refused by `Configure`
        }
    }
}

void Predictor::DropConfirmed(uint32_t confirmed_sequence) noexcept {
    if (confirmed_sequence == 0) {
        return; // 0 is "none yet", not a sequence number
    }
    // Serial arithmetic, so that the count can wrap around 2^32.
    while (pending_ > 0 && static_cast<int32_t>(history_[head_].sequence - confirmed_sequence) <= 0) {
        head_ = (head_ + 1) % kHistoryCapacity;
        --pending_;
    }
}

bool Predictor::Predict(uint32_t sequence, std::span<const uint8_t> input, double dt_seconds) {
    if (status_ != PredictionStatus::Active || input.size() > kMaxInputBytes) {
        return false;
    }
    if (!ApplyRule(state_, input, dt_seconds)) {
        Fail();
        return false;
    }
    if (pending_ == kHistoryCapacity) {
        head_ = (head_ + 1) % kHistoryCapacity; // the oldest makes room
        --pending_;
    }
    InputEntry& entry = history_[(head_ + pending_) % kHistoryCapacity];
    entry = InputEntry{};
    entry.sequence = sequence;
    entry.size = static_cast<uint8_t>(input.size());
    std::copy(input.begin(), input.end(), entry.bytes.begin());
    ++pending_;
    return true;
}

bool Predictor::Reconcile(uint64_t tick, const uint32_t* entity_words, uint32_t confirmed_sequence,
                          double dt_seconds) {
    if (status_ == PredictionStatus::Off || status_ == PredictionStatus::Failed) {
        return false;
    }
    if (any_tick_ && tick <= last_tick_) {
        return false;
    }
    any_tick_ = true;
    last_tick_ = tick;
    if (entity_words == nullptr) {
        head_ = 0;
        pending_ = 0;
        status_ = PredictionStatus::Waiting;
        return false;
    }
    DropConfirmed(confirmed_sequence);
    ReadState(entity_words, work_);
    for (uint32_t i = 0; i < pending_; ++i) {
        const InputEntry& entry = history_[(head_ + i) % kHistoryCapacity];
        if (!ApplyRule(work_, entry.Bytes(), dt_seconds)) {
            Fail();
            return false;
        }
    }
    state_.swap(work_);
    status_ = PredictionStatus::Active;
    return true;
}

bool Predictor::Displayed(std::span<double> out) const noexcept {
    if (status_ != PredictionStatus::Active || out.size() < components_) {
        return false;
    }
    for (uint32_t i = 0; i < components_; ++i) {
        out[i] = state_[i];
    }
    return true;
}

} // namespace tessera::net
