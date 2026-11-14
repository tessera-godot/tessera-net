// SPDX-License-Identifier: Apache-2.0
// Shared pieces for the interpolation tests: quantized states of the test entity types.
#pragma once

#include "replication_test_support.hpp"
#include "tessera/net/replication/property_codecs.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace interpolation_test {

using namespace replication_test;

// Property indices of the test mob type: a position, an angle, an integer, a bit mask and an enum.
inline constexpr uint32_t kPosition = 0;
inline constexpr uint32_t kAngle = 1;
inline constexpr uint32_t kHealth = 2;
inline constexpr uint32_t kBits = 3;
inline constexpr uint32_t kEnum = 4;

inline constexpr double kStep = 0.01; // the quantization step of the test positions
inline constexpr double kTwoPi = 6.283185307179586476925286766559;

/// The words of one quantized state of the mob type.
inline std::vector<uint32_t> MobState(const ReplicationSchema& schema, double x, double y = 0.0,
                                      double z = 0.0, float yaw = 0.0f, int32_t health = 0,
                                      uint8_t state = 0, uint16_t flags = 0) {
    TestColumns columns{};
    columns.x[0] = static_cast<float>(x);
    columns.y[0] = static_cast<float>(y);
    columns.z[0] = static_cast<float>(z);
    columns.yaw[0] = yaw;
    columns.health[0] = health;
    columns.state[0] = state;
    columns.flags[0] = flags;
    const TypeSchema* type = schema.Find(kMobType);
    std::vector<uint32_t> words(type->words, 0);
    for (const PropertyDescriptor& property : type->properties) {
        QuantizeProperty(property, reinterpret_cast<const std::byte*>(&columns), 0,
                         words.data() + property.word_offset);
    }
    return words;
}

/// The words of one quantized state of the score type (a single 64-bit integer).
inline std::vector<uint32_t> ScoreState(const ReplicationSchema& schema, int64_t value) {
    TestColumns columns{};
    columns.score[0] = value;
    const TypeSchema* type = schema.Find(kScoreType);
    std::vector<uint32_t> words(type->words, 0);
    for (const PropertyDescriptor& property : type->properties) {
        QuantizeProperty(property, reinterpret_cast<const std::byte*>(&columns), 0,
                         words.data() + property.word_offset);
    }
    return words;
}

/// The three coordinates of the position of a mob state.
inline void PositionOf(const ReplicationSchema& schema, const uint32_t* words, double out[3]) {
    const PropertyDescriptor& property = schema.Find(kMobType)->properties[kPosition];
    DequantizePosition(property, words + property.word_offset, out);
}

inline double XOf(const ReplicationSchema& schema, const uint32_t* words) {
    double out[3];
    PositionOf(schema, words, out);
    return out[0];
}

} // namespace interpolation_test
