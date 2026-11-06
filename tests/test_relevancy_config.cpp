// SPDX-License-Identifier: Apache-2.0
#include <cstdint>
#include <limits>
#include <stdexcept>

#include "tessera/net/relevancy/relevancy_config.hpp"
#include "tessera/net/relevancy/relevancy_types.hpp"
#include "test_support.hpp"

using tessera::net::RelevancyConfig;
using tessera::net::ValidateRelevancyConfig;

namespace {

bool Throws(const RelevancyConfig& config) {
    try {
        ValidateRelevancyConfig(config);
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

}  // namespace

TESSERA_TEST(default_relevancy_config_is_valid) {
    TESSERA_CHECK(!Throws(RelevancyConfig{}));
}

TESSERA_TEST(default_relevancy_config_matches_documented_values) {
    const RelevancyConfig config;
    TESSERA_CHECK(config.max_connections == 128);
    TESSERA_CHECK(config.recalc_interval_ticks == 3);
    TESSERA_CHECK(config.max_observers_per_connection == 4);
    TESSERA_CHECK(config.max_visible_per_connection == 65536);
    TESSERA_CHECK(config.types.size() == 256);
}

TESSERA_TEST(zero_range_is_valid) {
    RelevancyConfig config;
    config.types[7].range = 0.0f;
    TESSERA_CHECK(!Throws(config));
}

TESSERA_TEST(invalid_counts_throw) {
    RelevancyConfig config;
    config.recalc_interval_ticks = 0;
    TESSERA_CHECK(Throws(config));

    config = RelevancyConfig{};
    config.max_connections = 0;
    TESSERA_CHECK(Throws(config));

    config = RelevancyConfig{};
    config.max_observers_per_connection = 0;
    TESSERA_CHECK(Throws(config));

    config = RelevancyConfig{};
    config.max_visible_per_connection = 0;
    TESSERA_CHECK(Throws(config));

    config = RelevancyConfig{};
    config.max_visible_per_connection = (1u << 24) + 1;
    TESSERA_CHECK(Throws(config));
}

TESSERA_TEST(largest_allowed_visible_set_is_valid) {
    RelevancyConfig config;
    config.max_visible_per_connection = 1u << 24;
    TESSERA_CHECK(!Throws(config));
}

TESSERA_TEST(invalid_range_or_margin_throws) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (const float bad : {nan, -1.0f, inf}) {
        RelevancyConfig config;
        config.types[7].range = bad;
        TESSERA_CHECK(Throws(config));
    }
    for (const float bad : {nan, -1.0f, inf}) {
        RelevancyConfig config;
        config.types[7].hysteresis_margin = bad;
        TESSERA_CHECK(Throws(config));
    }
}
