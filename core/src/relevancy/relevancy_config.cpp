// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/relevancy/relevancy_config.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

namespace tessera::net {

namespace {

// Each connection reserves a bitmap with one bit per compact id; this keeps that bitmap small enough
// that a typo in the configuration cannot ask for gigabytes.
constexpr uint32_t kMaxVisibleLimit = 1u << 24;

bool IsNonNegativeFinite(float value) {
    return std::isfinite(value) && value >= 0.0f;
}

} // namespace

void ValidateRelevancyConfig(const RelevancyConfig& config) {
    if (config.max_connections == 0) {
        throw std::invalid_argument("RelevancyConfig: max_connections must be at least 1");
    }
    if (config.recalc_interval_ticks == 0) {
        throw std::invalid_argument("RelevancyConfig: recalc_interval_ticks must be at least 1");
    }
    if (config.max_observers_per_connection == 0) {
        throw std::invalid_argument("RelevancyConfig: max_observers_per_connection must be at least 1");
    }
    if (config.max_visible_per_connection == 0 || config.max_visible_per_connection > kMaxVisibleLimit) {
        throw std::invalid_argument("RelevancyConfig: max_visible_per_connection must be between 1 and " +
                                    std::to_string(kMaxVisibleLimit));
    }
    if (!IsNonNegativeFinite(config.observer_tolerance)) {
        throw std::invalid_argument(
            "RelevancyConfig: observer_tolerance must be a finite number, zero or more");
    }
    for (size_t type = 0; type < config.types.size(); ++type) {
        const TypeRelevancy& rule = config.types[type];
        if (!IsNonNegativeFinite(rule.range)) {
            throw std::invalid_argument("RelevancyConfig: types[" + std::to_string(type) +
                                        "].range must be a finite number, zero or more");
        }
        if (!IsNonNegativeFinite(rule.hysteresis_margin)) {
            throw std::invalid_argument("RelevancyConfig: types[" + std::to_string(type) +
                                        "].hysteresis_margin must be a finite number, zero or more");
        }
    }
}

} // namespace tessera::net
