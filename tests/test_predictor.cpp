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
#include <stdexcept>
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

namespace {

// A rule for the properties {position, angle}: the state is x, y, z and the angle. The first three bytes of
// an input move x, z and the angle by that many times a scale.
PredictionRule Scaled(double x_scale, double z_scale, double angle_scale) {
    return [=](std::span<double> state, std::span<const uint8_t> input, double) {
        const auto at = [&input](size_t i) { return i < input.size() ? static_cast<double>(input[i]) : 0.0; };
        state[0] += at(0) * x_scale;
        state[2] += at(1) * z_scale;
        state[3] += at(2) * angle_scale;
        return true;
    };
}

PredictionConfig PositionAndAngle(PredictionRule rule, double smoothing, double snap = 2.0) {
    PredictionConfig config = Config(std::move(rule), {kPosition, kAngle});
    config.smoothing_seconds = smoothing;
    config.snap_distance = snap;
    return config;
}

constexpr double kDt = 1.0 / 30.0;

// The player's rule moves x by half of what the server's does. The server's state after the first input is 1
// unit further than the player's, so the error of that correction is 0.5.
struct Slide {
    Source source;
    Predictor predictor;

    explicit Slide(const ReplicationSchema& schema, double smoothing = 0.1, double snap = 2.0)
        : source(schema), predictor(schema) {
        TESSERA_CHECK(predictor.Configure(PositionAndAngle(Scaled(0.5, 0.5, 0.0), smoothing, snap), nullptr));
        TESSERA_CHECK(predictor.Reconcile(1, source.Set(100.0, 0.0, 50.0), 0, kDt));
    }
    double X() const {
        double out[4] = {};
        TESSERA_CHECK(predictor.Displayed(out));
        return out[0];
    }
};

} // namespace

TESSERA_TEST(a_different_rule_gives_an_error_that_slides_to_zero_over_the_smoothing_time) {
    const ReplicationSchema schema = MakeTestSchema();
    {
        Slide slide(schema);
        TESSERA_CHECK(slide.predictor.Predict(1, std::vector<uint8_t>{1}, kDt));
        TESSERA_CHECK(Near(slide.X(), 100.5, kStep));
        TESSERA_CHECK(slide.predictor.Reconcile(2, slide.source.Set(101.0, 0.0, 50.0), 1, kDt));
        TESSERA_CHECK(Near(slide.predictor.Stats().last_error, 0.5, 0.001));
        TESSERA_CHECK(slide.predictor.Stats().reconciliations == 2 && slide.predictor.Stats().snaps == 0);
        // What was shown does not jump: the correction starts from it.
        TESSERA_CHECK(Near(slide.X(), 100.5, 0.002));
        const double expected[] = {100.625, 100.75, 100.875, 101.0};
        double previous = slide.X();
        for (const double want : expected) {
            slide.predictor.Advance(0.025);
            const double now = slide.X();
            TESSERA_CHECK(Near(now, want, 0.002));
            TESSERA_CHECK(now >= previous - 1e-9 && now <= 101.0 + 0.002); // never past the corrected value
            previous = now;
        }
        slide.predictor.Advance(0.025); // and it stays there
        TESSERA_CHECK(Near(slide.X(), 101.0, 0.002));
    }
    {
        // A correction that arrives in the middle of a slide starts from what is being shown.
        Slide slide(schema);
        TESSERA_CHECK(slide.predictor.Predict(1, std::vector<uint8_t>{1}, kDt));
        TESSERA_CHECK(slide.predictor.Reconcile(2, slide.source.Set(101.0, 0.0, 50.0), 1, kDt));
        slide.predictor.Advance(0.05); // half way: 100.75
        TESSERA_CHECK(Near(slide.X(), 100.75, 0.002));
        TESSERA_CHECK(slide.predictor.Predict(2, std::vector<uint8_t>{1}, kDt)); // 101.5, less what is left
        TESSERA_CHECK(Near(slide.X(), 101.25, 0.002));
        TESSERA_CHECK(slide.predictor.Reconcile(3, slide.source.Set(102.0, 0.0, 50.0), 2, kDt));
        TESSERA_CHECK(Near(slide.predictor.Stats().last_error, 0.75, 0.001));
        TESSERA_CHECK(Near(slide.X(), 101.25, 0.002)); // no jump
        slide.predictor.Advance(0.1);
        TESSERA_CHECK(Near(slide.X(), 102.0, 0.002));
    }
}

TESSERA_TEST(an_error_above_the_snap_distance_is_zeroed_at_once_and_counted) {
    const ReplicationSchema schema = MakeTestSchema();
    struct Outcome {
        double error;
        uint64_t snaps;
        double x;
        double z;
    };
    const auto run = [&schema](double snap, uint8_t x_input, uint8_t z_input) {
        Source source(schema);
        Predictor predictor(schema);
        TESSERA_CHECK(predictor.Configure(PositionAndAngle(Scaled(0.5, 0.5, 1.0), 0.1, snap), nullptr));
        TESSERA_CHECK(predictor.Reconcile(1, source.Set(100.0, 0.0, 50.0), 0, kDt));
        TESSERA_CHECK(predictor.Predict(1, std::vector<uint8_t>{x_input, z_input, 1}, kDt));
        // The server moved x and z by the full input and did not turn at all.
        TESSERA_CHECK(predictor.Reconcile(2, source.Set(100.0 + x_input, 0.0, 50.0 + z_input, 0.0f), 1, kDt));
        double shown[4] = {};
        TESSERA_CHECK(predictor.Displayed(shown));
        return Outcome{predictor.Stats().last_error, predictor.Stats().snaps, shown[0], shown[2]};
    };
    // Half of (3, 4) is (1.5, 2): the error is 2.5, above 2. The angle is wrong by a whole radian, which is
    // not a distance and does not count.
    {
        const Outcome far = run(2.0, 3, 4);
        TESSERA_CHECK(Near(far.error, 2.5, 0.01));
        TESSERA_CHECK(far.snaps == 1);
        // Already at the corrected value.
        TESSERA_CHECK(Near(far.x, 103.0, 0.002) && Near(far.z, 54.0, 0.002));
    }
    // Half of (1, 1) is (0.5, 0.5): 0.71 slides.
    {
        const Outcome near = run(2.0, 1, 1);
        TESSERA_CHECK(Near(near.error, std::sqrt(0.5), 0.01));
        TESSERA_CHECK(near.snaps == 0);
        TESSERA_CHECK(Near(near.x, 100.5, 0.002) && Near(near.z, 50.5, 0.002)); // still where it was shown
    }
    // The limit itself slides; anything a little under it jumps. The error is read first, with room to spare.
    const double measured = run(100.0, 3, 4).error;
    TESSERA_CHECK(run(measured, 3, 4).snaps == 0);
    TESSERA_CHECK(run(std::nextafter(measured, 0.0), 3, 4).snaps == 1);
}

TESSERA_TEST(smoothing_of_zero_seconds_corrects_at_once) {
    const ReplicationSchema schema = MakeTestSchema();
    Slide slide(schema, 0.0);
    TESSERA_CHECK(slide.predictor.Predict(1, std::vector<uint8_t>{1}, kDt));
    TESSERA_CHECK(Near(slide.X(), 100.5, kStep));
    TESSERA_CHECK(slide.predictor.Reconcile(2, slide.source.Set(101.0, 0.0, 50.0), 1, kDt));
    TESSERA_CHECK(Near(slide.X(), 101.0, 0.002));
    TESSERA_CHECK(Near(slide.predictor.Stats().last_error, 0.5, 0.001) && slide.predictor.Stats().snaps == 0);
}

TESSERA_TEST(a_slide_ends_exactly_at_the_predicted_value) {
    const ReplicationSchema schema = MakeTestSchema();
    Slide slide(schema);
    Slide twin(schema, 0.0); // the same inputs, corrected at once: the value the slide has to end at
    for (Slide* s : {&slide, &twin}) {
        TESSERA_CHECK(s->predictor.Predict(1, std::vector<uint8_t>{1}, kDt));
        TESSERA_CHECK(s->predictor.Reconcile(2, s->source.Set(101.0, 0.0, 50.0), 1, kDt));
    }
    slide.predictor.Advance(0.04);
    const double middle = slide.X();
    TESSERA_CHECK(middle > 100.5 && middle < 101.0);
    // Time that does not make sense changes nothing.
    slide.predictor.Advance(-1.0);
    slide.predictor.Advance(std::numeric_limits<double>::quiet_NaN());
    slide.predictor.Advance(-std::numeric_limits<double>::infinity());
    TESSERA_CHECK(slide.X() == middle);
    slide.predictor.Advance(10.0);
    TESSERA_CHECK(slide.X() == twin.X());
    slide.predictor.Advance(1e9);
    TESSERA_CHECK(slide.X() == twin.X());
}

TESSERA_TEST(a_full_history_pauses_jumps_and_resumes_at_the_next_reconcile) {
    const ReplicationSchema schema = MakeTestSchema();
    Source source(schema);
    Predictor predictor(schema);
    auto seen = std::make_shared<Seen>();
    PredictionConfig config = Config(MoveXByInput(seen));
    config.smoothing_seconds = 0.1;
    TESSERA_CHECK(predictor.Configure(config, nullptr));
    TESSERA_CHECK(predictor.Reconcile(1, source.Set(10.0, 0.0, 0.0), 0, kDt));
    for (uint32_t sequence = 1; sequence <= Predictor::kHistoryCapacity; ++sequence) {
        TESSERA_CHECK(predictor.Predict(sequence, Byte(1), kDt));
    }
    TESSERA_CHECK(predictor.Status() == PredictionStatus::Active);
    TESSERA_CHECK(predictor.Pending() == Predictor::kHistoryCapacity);
    const size_t calls = seen->first_bytes.size();
    // One more with nothing confirmed does not fit.
    TESSERA_CHECK(!predictor.Predict(Predictor::kHistoryCapacity + 1, Byte(1), kDt));
    TESSERA_CHECK(predictor.Status() == PredictionStatus::Paused);
    TESSERA_CHECK(predictor.Stats().history_overflows == 1);
    TESSERA_CHECK(predictor.Pending() == 0 && seen->first_bytes.size() == calls); // the rule did not run
    double out[3] = {};
    TESSERA_CHECK(!predictor.Displayed(out)); // the game falls back to what the server says
    for (uint32_t i = 0; i < 1000; ++i) {
        TESSERA_CHECK(!predictor.Predict(100 + i, Byte(1), kDt));
        TESSERA_CHECK(predictor.Pending() <= Predictor::kHistoryCapacity);
    }
    TESSERA_CHECK(predictor.Stats().history_overflows == 1 && seen->first_bytes.size() == calls);
    // A state that is not newer than the last one seen does not start it again.
    TESSERA_CHECK(!predictor.Reconcile(1, source.Set(40.0, 0.0, 0.0), 0, kDt));
    TESSERA_CHECK(predictor.Status() == PredictionStatus::Paused);
    // The next state of the server starts it again, from that state.
    TESSERA_CHECK(predictor.Reconcile(2, source.Set(55.0, 0.0, 0.0), 1000, kDt));
    TESSERA_CHECK(predictor.Status() == PredictionStatus::Active && predictor.Pending() == 0);
    TESSERA_CHECK(predictor.Displayed(out) && Near(out[0], 55.0, kStep));
    TESSERA_CHECK(predictor.Predict(2000, Byte(2), kDt));
    TESSERA_CHECK(predictor.Displayed(out) && Near(out[0], 57.0, kStep));
}

TESSERA_TEST(a_rule_that_returns_false_stops_the_prediction) {
    const ReplicationSchema schema = MakeTestSchema();
    Source source(schema);
    {
        Predictor predictor(schema);
        PredictionRule refuses = [](std::span<double>, std::span<const uint8_t>, double) { return false; };
        TESSERA_CHECK(predictor.Configure(Config(refuses), nullptr));
        // No inputs yet: nothing to refuse.
        TESSERA_CHECK(predictor.Reconcile(1, source.Set(1.0, 0.0, 0.0), 0, kDt));
        TESSERA_CHECK(!predictor.Predict(1, Byte(1), kDt));
        TESSERA_CHECK(predictor.Status() == PredictionStatus::Failed && predictor.Stats().rule_failures == 1);
        double out[3] = {};
        TESSERA_CHECK(!predictor.Displayed(out));
        TESSERA_CHECK(!predictor.Predict(2, Byte(1), kDt));
        TESSERA_CHECK(!predictor.Reconcile(2, source.Set(2.0, 0.0, 0.0), 0, kDt));
        TESSERA_CHECK(predictor.Stats().rule_failures == 1); // counted once
        predictor.Reset(); // a new connection does not bring it back
        TESSERA_CHECK(predictor.Status() == PredictionStatus::Failed);
        // Only configuring again does.
        TESSERA_CHECK(predictor.Configure(Config(MoveXByInput()), nullptr));
        TESSERA_CHECK(predictor.Status() == PredictionStatus::Waiting &&
                      predictor.Stats().rule_failures == 0);
    }
    {
        // A rule that works for the input and fails when a reconciliation applies it again.
        Predictor predictor(schema);
        auto calls = std::make_shared<int>(0);
        PredictionRule second_time_fails = [calls](std::span<double> state, std::span<const uint8_t>,
                                                   double) {
            state[0] += 1.0;
            return ++*calls < 2;
        };
        TESSERA_CHECK(predictor.Configure(Config(second_time_fails), nullptr));
        TESSERA_CHECK(predictor.Reconcile(1, source.Set(1.0, 0.0, 0.0), 0, kDt));
        TESSERA_CHECK(predictor.Predict(1, Byte(1), kDt));
        TESSERA_CHECK(!predictor.Reconcile(2, source.Set(1.0, 0.0, 0.0), 0, kDt));
        TESSERA_CHECK(predictor.Status() == PredictionStatus::Failed && predictor.Stats().rule_failures == 1);
    }
}

TESSERA_TEST(a_rule_that_throws_is_caught_and_stops_the_prediction) {
    const ReplicationSchema schema = MakeTestSchema();
    Source source(schema);
    for (const int kind : {0, 1}) {
        Predictor predictor(schema);
        PredictionRule throws = [kind](std::span<double>, std::span<const uint8_t>, double) -> bool {
            if (kind == 0) {
                throw std::runtime_error("the rule broke");
            }
            throw 42;
        };
        TESSERA_CHECK(predictor.Configure(Config(throws), nullptr));
        TESSERA_CHECK(predictor.Reconcile(1, source.Set(1.0, 0.0, 0.0), 0, kDt));
        bool escaped = false;
        try {
            TESSERA_CHECK(!predictor.Predict(1, Byte(1), kDt));
        } catch (...) {
            escaped = true;
        }
        TESSERA_CHECK(!escaped);
        TESSERA_CHECK(predictor.Status() == PredictionStatus::Failed && predictor.Stats().rule_failures == 1);
    }
    // And during a reconciliation.
    Predictor predictor(schema);
    auto calls = std::make_shared<int>(0);
    PredictionRule later = [calls](std::span<double>, std::span<const uint8_t>, double) -> bool {
        if (++*calls > 1) {
            throw std::runtime_error("the rule broke");
        }
        return true;
    };
    TESSERA_CHECK(predictor.Configure(Config(later), nullptr));
    TESSERA_CHECK(predictor.Reconcile(1, source.Set(1.0, 0.0, 0.0), 0, kDt));
    TESSERA_CHECK(predictor.Predict(1, Byte(1), kDt));
    bool escaped = false;
    try {
        TESSERA_CHECK(!predictor.Reconcile(2, source.Set(1.0, 0.0, 0.0), 0, kDt));
    } catch (...) {
        escaped = true;
    }
    TESSERA_CHECK(!escaped && predictor.Status() == PredictionStatus::Failed);
}

TESSERA_TEST(a_rule_that_leaves_a_number_that_is_not_finite_stops_the_prediction) {
    const ReplicationSchema schema = MakeTestSchema();
    Source source(schema);
    const double inf = std::numeric_limits<double>::infinity();
    for (const double bad : {std::numeric_limits<double>::quiet_NaN(), inf, -inf}) {
        Predictor predictor(schema);
        PredictionRule rule = [bad](std::span<double> state, std::span<const uint8_t>, double) {
            state[1] = bad; // the y component, not the one a test would look at first
            return true;    // and it says all is well
        };
        TESSERA_CHECK(predictor.Configure(Config(rule), nullptr));
        TESSERA_CHECK(predictor.Reconcile(1, source.Set(1.0, 0.0, 0.0), 0, kDt));
        TESSERA_CHECK(!predictor.Predict(1, Byte(1), kDt));
        TESSERA_CHECK(predictor.Status() == PredictionStatus::Failed && predictor.Stats().rule_failures == 1);
        double out[3] = {};
        TESSERA_CHECK(!predictor.Displayed(out));
    }
    // The same during a reconciliation: a rule that turns bad on its second call.
    Predictor predictor(schema);
    auto calls = std::make_shared<int>(0);
    PredictionRule later = [calls](std::span<double> state, std::span<const uint8_t>, double) {
        if (++*calls > 1) {
            state[0] = std::numeric_limits<double>::quiet_NaN();
        }
        return true;
    };
    TESSERA_CHECK(predictor.Configure(Config(later), nullptr));
    TESSERA_CHECK(predictor.Reconcile(1, source.Set(1.0, 0.0, 0.0), 0, kDt));
    TESSERA_CHECK(predictor.Predict(1, Byte(1), kDt));
    TESSERA_CHECK(!predictor.Reconcile(2, source.Set(1.0, 0.0, 0.0), 0, kDt));
    TESSERA_CHECK(predictor.Status() == PredictionStatus::Failed);
}
