// SPDX-License-Identifier: Apache-2.0
// The predictor on its own: the configuration, reading the state, applying the rule to the inputs and
// replaying the ones the server has not used yet.
#include "replication_test_support.hpp"
#include "tessera/net/predictor.hpp"
#include "tessera/net/replication/property_codecs.hpp"
#include "test_support.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace tessera::net;
using namespace replication_test;

namespace {

// Property indices of the test type: a position, an angle, an integer, a bit mask and an enum.
constexpr uint32_t kPosition = 0;
constexpr uint32_t kAngle = 1;
constexpr uint32_t kHealth = 2;
constexpr uint32_t kBits = 3;
constexpr uint32_t kEnum = 4;

constexpr double kStep = 0.01;

// The quantized state of one entity of the test type, as the server would have sent it.
struct Source {
    const ReplicationSchema& schema;
    std::unique_ptr<TestColumns> columns = std::make_unique<TestColumns>();
    std::vector<uint32_t> words;

    explicit Source(const ReplicationSchema& s) : schema(s), words(s.Find(kMobType)->words, 0) {
    }

    const uint32_t* Set(double x, double y, double z, float yaw = 0.0f, int32_t health = 0,
                        uint8_t state = 0) {
        columns->x[0] = static_cast<float>(x);
        columns->y[0] = static_cast<float>(y);
        columns->z[0] = static_cast<float>(z);
        columns->yaw[0] = yaw;
        columns->health[0] = health;
        columns->state[0] = state;
        columns->flags[0] = 0;
        const TypeSchema* type = schema.Find(kMobType);
        for (const PropertyDescriptor& property : type->properties) {
            QuantizeProperty(property, reinterpret_cast<const std::byte*>(columns.get()), 0,
                             words.data() + property.word_offset);
        }
        return words.data();
    }
};

// What a test rule saw.
struct Seen {
    std::vector<uint8_t> first_bytes; // the first byte of each input, in the order of the calls
    std::vector<size_t> sizes;
    std::vector<double> dts;
    std::vector<size_t> state_sizes;
};

// A rule that moves x by `input[0]` times `scale` (so 1 unit per input byte with the default).
PredictionRule MoveXByInput(std::shared_ptr<Seen> seen = nullptr, double scale = 1.0) {
    return [seen, scale](std::span<double> state, std::span<const uint8_t> input, double dt) {
        if (seen) {
            seen->first_bytes.push_back(input.empty() ? 0 : input[0]);
            seen->sizes.push_back(input.size());
            seen->dts.push_back(dt);
            seen->state_sizes.push_back(state.size());
        }
        state[0] += (input.empty() ? 0.0 : static_cast<double>(input[0])) * scale;
        return true;
    };
}

PredictionConfig Config(PredictionRule rule, std::vector<uint32_t> properties = {kPosition}) {
    PredictionConfig config;
    config.entity = MakeEntity(kMobType, 1);
    config.properties = std::move(properties);
    config.rule = std::move(rule);
    config.smoothing_seconds = 0.0; // these tests look at the corrected state, not at the slide
    return config;
}

std::vector<uint8_t> Byte(uint32_t value) {
    return {static_cast<uint8_t>(value & 0xFF)};
}

bool Near(double a, double b, double tolerance) {
    return std::fabs(a - b) <= tolerance;
}

} // namespace

TESSERA_TEST(a_config_without_a_rule_an_entity_or_properties_is_refused) {
    const ReplicationSchema schema = MakeTestSchema();
    Predictor predictor(schema);
    std::string error;

    PredictionConfig no_rule = Config(nullptr);
    TESSERA_CHECK(!predictor.Configure(no_rule, &error) && !error.empty());
    TESSERA_CHECK(predictor.Status() == PredictionStatus::Off);

    error.clear();
    PredictionConfig no_entity = Config(MoveXByInput());
    no_entity.entity = EntityId::Null();
    TESSERA_CHECK(!predictor.Configure(no_entity, &error) && !error.empty());

    error.clear();
    PredictionConfig no_properties = Config(MoveXByInput(), {});
    TESSERA_CHECK(!predictor.Configure(no_properties, &error) && !error.empty());
    TESSERA_CHECK(predictor.Status() == PredictionStatus::Off);

    // Without somewhere to put the reason it is still refused.
    TESSERA_CHECK(!predictor.Configure(no_rule, nullptr));
    TESSERA_CHECK(predictor.Configure(Config(MoveXByInput()), nullptr));
    TESSERA_CHECK(predictor.Status() == PredictionStatus::Waiting);
}

TESSERA_TEST(a_config_with_an_unknown_repeated_or_bits_property_is_refused) {
    const ReplicationSchema schema = MakeTestSchema();
    Predictor predictor(schema);
    std::string error;

    PredictionConfig unknown_type = Config(MoveXByInput());
    unknown_type.entity = MakeEntity(kUnknownType, 1);
    TESSERA_CHECK(!predictor.Configure(unknown_type, &error) && !error.empty());

    error.clear();
    TESSERA_CHECK(!predictor.Configure(Config(MoveXByInput(), {9}), &error) && !error.empty());
    error.clear();
    TESSERA_CHECK(!predictor.Configure(Config(MoveXByInput(), {kPosition, kPosition}), &error) &&
                  !error.empty());
    error.clear();
    TESSERA_CHECK(!predictor.Configure(Config(MoveXByInput(), {kPosition, kBits}), &error) && !error.empty());
    TESSERA_CHECK(predictor.Status() == PredictionStatus::Off);

    // A refused configuration leaves the one that worked in place.
    TESSERA_CHECK(predictor.Configure(Config(MoveXByInput()), nullptr));
    TESSERA_CHECK(!predictor.Configure(Config(MoveXByInput(), {kBits}), &error));
    TESSERA_CHECK(predictor.Status() == PredictionStatus::Waiting && predictor.Components() == 3);
}

TESSERA_TEST(smoothing_and_snap_outside_their_range_are_refused) {
    const ReplicationSchema schema = MakeTestSchema();
    Predictor predictor(schema);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (const double smoothing : {-1.0, 5.1, nan, inf, -inf}) {
        PredictionConfig config = Config(MoveXByInput());
        config.smoothing_seconds = smoothing;
        std::string error;
        TESSERA_CHECK(!predictor.Configure(config, &error) && !error.empty());
    }
    for (const double snap : {0.0, -1.0, nan, inf}) {
        PredictionConfig config = Config(MoveXByInput());
        config.snap_distance = snap;
        std::string error;
        TESSERA_CHECK(!predictor.Configure(config, &error) && !error.empty());
    }
    TESSERA_CHECK(predictor.Status() == PredictionStatus::Off);
    for (const double smoothing : {0.0, 0.1, 5.0}) {
        PredictionConfig config = Config(MoveXByInput());
        config.smoothing_seconds = smoothing;
        TESSERA_CHECK(predictor.Configure(config, nullptr));
    }
}

TESSERA_TEST(components_are_read_from_the_quantized_words) {
    const ReplicationSchema schema = MakeTestSchema();
    Source source(schema);
    const uint32_t* words = source.Set(12.345, -1.5, 987.65, 1.25f, -37, 5);
    {
        Predictor predictor(schema);
        TESSERA_CHECK(
            predictor.Configure(Config(MoveXByInput(), {kPosition, kAngle, kHealth, kEnum}), nullptr));
        TESSERA_CHECK(predictor.Components() == 6);
        TESSERA_CHECK(predictor.Reconcile(1, words, 0, 1.0 / 30.0));
        TESSERA_CHECK(predictor.Status() == PredictionStatus::Active);
        double out[6] = {};
        TESSERA_CHECK(predictor.Displayed(out));
        TESSERA_CHECK(Near(out[0], 12.345, kStep / 2 + 1e-6) && Near(out[1], -1.5, kStep / 2 + 1e-6) &&
                      Near(out[2], 987.65, kStep / 2 + 1e-6));
        TESSERA_CHECK(Near(out[3], 1.25, 2 * 3.14159265358979 / 1024.0)); // 10 bits around the circle
        TESSERA_CHECK(out[4] == -37.0 && out[5] == 5.0);
    }
    {
        // The order of the numbers follows the order of the properties, not the order of the schema.
        Predictor predictor(schema);
        TESSERA_CHECK(predictor.Configure(Config(MoveXByInput(), {kEnum, kPosition}), nullptr));
        TESSERA_CHECK(predictor.Components() == 4);
        TESSERA_CHECK(predictor.Reconcile(1, words, 0, 1.0 / 30.0));
        double out[4] = {};
        TESSERA_CHECK(predictor.Displayed(out));
        TESSERA_CHECK(out[0] == 5.0 && Near(out[1], 12.345, kStep / 2 + 1e-6) &&
                      Near(out[3], 987.65, kStep / 2 + 1e-6));
    }
}

TESSERA_TEST(predict_applies_the_rule_once_with_the_input_bytes_and_dt) {
    const ReplicationSchema schema = MakeTestSchema();
    Source source(schema);
    Predictor predictor(schema);
    auto seen = std::make_shared<Seen>();
    TESSERA_CHECK(predictor.Configure(Config(MoveXByInput(seen)), nullptr));
    TESSERA_CHECK(predictor.Reconcile(1, source.Set(10.0, 0.0, 0.0), 0, 1.0 / 30.0));
    const std::vector<uint8_t> input = {7, 9};
    TESSERA_CHECK(predictor.Predict(1, input, 0.125));
    TESSERA_CHECK(seen->first_bytes == std::vector<uint8_t>{7} && seen->sizes == std::vector<size_t>{2});
    TESSERA_CHECK(seen->dts == std::vector<double>{0.125} && seen->state_sizes == std::vector<size_t>{3});
    double out[3] = {};
    TESSERA_CHECK(predictor.Displayed(out) && Near(out[0], 17.0, kStep) && Near(out[1], 0.0, kStep));
    TESSERA_CHECK(predictor.Pending() == 1);
}

TESSERA_TEST(predict_before_the_first_state_does_nothing) {
    const ReplicationSchema schema = MakeTestSchema();
    Predictor predictor(schema);
    auto seen = std::make_shared<Seen>();
    TESSERA_CHECK(!predictor.Predict(1, Byte(1), 0.1)); // not configured
    TESSERA_CHECK(predictor.Configure(Config(MoveXByInput(seen)), nullptr));
    TESSERA_CHECK(predictor.Status() == PredictionStatus::Waiting);
    TESSERA_CHECK(!predictor.Predict(1, Byte(1), 0.1));
    TESSERA_CHECK(seen->first_bytes.empty() && predictor.Pending() == 0);
    double out[3] = {};
    TESSERA_CHECK(!predictor.Displayed(out));
}

TESSERA_TEST(reconcile_replays_only_the_inputs_after_the_confirmed_one) {
    const ReplicationSchema schema = MakeTestSchema();
    Source source(schema);
    Predictor predictor(schema);
    auto seen = std::make_shared<Seen>();
    TESSERA_CHECK(predictor.Configure(Config(MoveXByInput(seen)), nullptr));
    TESSERA_CHECK(predictor.Reconcile(1, source.Set(100.0, 0.0, 0.0), 0, 0.1));
    for (uint32_t sequence = 1; sequence <= 5; ++sequence) {
        TESSERA_CHECK(predictor.Predict(sequence, Byte(sequence), 0.1));
    }
    TESSERA_CHECK(predictor.Pending() == 5);
    seen->first_bytes.clear();
    // The server says its state of tick 2 has the inputs up to 3: only 4 and 5 are applied again.
    TESSERA_CHECK(predictor.Reconcile(2, source.Set(100.0, 0.0, 0.0), 3, 0.1));
    TESSERA_CHECK((seen->first_bytes == std::vector<uint8_t>{4, 5}));
    TESSERA_CHECK(predictor.Pending() == 2);
    double out[3] = {};
    TESSERA_CHECK(predictor.Displayed(out) && Near(out[0], 109.0, kStep));
    // The same tick again is not used a second time.
    seen->first_bytes.clear();
    TESSERA_CHECK(!predictor.Reconcile(2, source.Set(500.0, 0.0, 0.0), 5, 0.1));
    TESSERA_CHECK(seen->first_bytes.empty() && predictor.Displayed(out) && Near(out[0], 109.0, kStep));
}

TESSERA_TEST(reconcile_with_confirmed_0_confirms_nothing) {
    const ReplicationSchema schema = MakeTestSchema();
    Source source(schema);
    Predictor predictor(schema);
    auto seen = std::make_shared<Seen>();
    TESSERA_CHECK(predictor.Configure(Config(MoveXByInput(seen)), nullptr));
    TESSERA_CHECK(predictor.Reconcile(1, source.Set(100.0, 0.0, 0.0), 0, 0.1));
    for (uint32_t sequence = 1; sequence <= 5; ++sequence) {
        TESSERA_CHECK(predictor.Predict(sequence, Byte(sequence), 0.1));
    }
    seen->first_bytes.clear();
    // The server has not used any input yet: all five are applied again.
    TESSERA_CHECK(predictor.Reconcile(2, source.Set(100.0, 0.0, 0.0), 0, 0.1));
    TESSERA_CHECK((seen->first_bytes == std::vector<uint8_t>{1, 2, 3, 4, 5}));
    TESSERA_CHECK(predictor.Pending() == 5);
    double out[3] = {};
    TESSERA_CHECK(predictor.Displayed(out) && Near(out[0], 115.0, kStep));
}

TESSERA_TEST(with_the_same_rule_the_reconciled_value_is_within_a_quantization_step) {
    const ReplicationSchema schema = MakeTestSchema();
    Source source(schema);
    Predictor predictor(schema);
    // One input moves x by a number that has no short binary form, like a speed times a tick.
    const double per_input = 8.0 / 30.0 * 0.9137;
    PredictionRule rule = [per_input](std::span<double> state, std::span<const uint8_t> input, double) {
        state[0] += static_cast<double>(input[0]) * per_input;
        return true;
    };
    TESSERA_CHECK(predictor.Configure(Config(rule), nullptr));
    const double start = 3.0;
    TESSERA_CHECK(predictor.Reconcile(1, source.Set(start, 0.0, 0.0), 0, 1.0 / 30.0));
    double worst = 0.0;
    for (uint32_t sequence = 1; sequence <= 200; ++sequence) {
        TESSERA_CHECK(predictor.Predict(sequence, std::vector<uint8_t>{1}, 1.0 / 30.0));
        // The server is two inputs behind the player. Its own number is not quantized; the state it sends is.
        const uint32_t used = sequence > 2 ? sequence - 2 : 0;
        const double server_x = start + used * per_input;
        TESSERA_CHECK(predictor.Reconcile(sequence + 1, source.Set(server_x, 0.0, 0.0), used, 1.0 / 30.0));
        double out[3] = {};
        TESSERA_CHECK(predictor.Displayed(out));
        worst = std::max(worst, std::fabs(out[0] - (start + sequence * per_input)));
    }
    TESSERA_CHECK(worst < kStep);
}

TESSERA_TEST(a_confirmed_sequence_that_wraps_past_uint32_is_applied_in_order) {
    const ReplicationSchema schema = MakeTestSchema();
    Source source(schema);
    Predictor predictor(schema);
    auto seen = std::make_shared<Seen>();
    TESSERA_CHECK(predictor.Configure(Config(MoveXByInput(seen)), nullptr));
    TESSERA_CHECK(predictor.Reconcile(1, source.Set(0.0, 0.0, 0.0), 0, 0.1));
    for (const uint32_t sequence : {0xFFFFFFFEu, 0xFFFFFFFFu, 1u, 2u}) {
        TESSERA_CHECK(predictor.Predict(sequence, Byte(sequence), 0.1));
    }
    seen->first_bytes.clear();
    TESSERA_CHECK(predictor.Reconcile(2, source.Set(0.0, 0.0, 0.0), 0xFFFFFFFFu, 0.1));
    TESSERA_CHECK((seen->first_bytes == std::vector<uint8_t>{1, 2}));
    TESSERA_CHECK(predictor.Pending() == 2);
}

TESSERA_TEST(reset_discards_the_state_but_keeps_the_configuration) {
    const ReplicationSchema schema = MakeTestSchema();
    Source source(schema);
    Predictor predictor(schema);
    TESSERA_CHECK(predictor.Configure(Config(MoveXByInput()), nullptr));
    TESSERA_CHECK(predictor.Reconcile(5, source.Set(10.0, 0.0, 0.0), 0, 0.1));
    TESSERA_CHECK(predictor.Predict(1, Byte(1), 0.1));
    predictor.Reset();
    TESSERA_CHECK(predictor.Status() == PredictionStatus::Waiting && predictor.Pending() == 0);
    double out[3] = {};
    TESSERA_CHECK(!predictor.Displayed(out) && !predictor.Predict(2, Byte(1), 0.1));
    TESSERA_CHECK(predictor.Config().entity == MakeEntity(kMobType, 1) && predictor.Components() == 3);
    // A connection that starts over has smaller ticks again: the old mark is gone as well.
    TESSERA_CHECK(predictor.Reconcile(1, source.Set(20.0, 0.0, 0.0), 0, 0.1));
    TESSERA_CHECK(predictor.Status() == PredictionStatus::Active);
    TESSERA_CHECK(predictor.Displayed(out) && Near(out[0], 20.0, kStep));
    predictor.Clear();
    TESSERA_CHECK(predictor.Status() == PredictionStatus::Off);
}
