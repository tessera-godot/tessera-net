// SPDX-License-Identifier: Apache-2.0
// The client's prediction: what it does with the player's inputs and with the state buffers that confirm
// them.
#include "network_test_support.hpp"
#include "test_support.hpp"

#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace network_test;

namespace {

constexpr double kStep = 0.01;
constexpr double kTickDt = 1.0 / 30.0;

bool Near(double a, double b, double tolerance = kStep) {
    return std::fabs(a - b) <= tolerance;
}

// What a test rule saw.
struct Seen {
    int calls = 0;
    std::vector<double> dts;
};

// Moves x by `input[0]` times `scale` (an input of 1 byte per step), and notes what it was given.
PredictionRule MoveX(std::shared_ptr<Seen> seen, double scale = 1.0) {
    return [seen, scale](std::span<double> state, std::span<const uint8_t> input, double dt) {
        if (seen) {
            ++seen->calls;
            seen->dts.push_back(dt);
        }
        state[0] += (input.empty() ? 0.0 : static_cast<double>(input[0])) * scale;
        return true;
    };
}

// A client that is connected (tick rate 30), a source of real state buffers with one entity that is visible,
// and a prediction of that entity's position.
struct Scene {
    ClientHarness h;
    ReplicationSource source{h.schema};
    EntityId entity = source.Spawn(5.0f, 0.0f, 0.0f);
    std::vector<VisibilityEvent> enter{Enter(entity, 0)};
    std::shared_ptr<Seen> seen = std::make_shared<Seen>();
    uint64_t batches = 0;
    uint64_t tick = 0;

    explicit Scene(bool connect = true) {
        source.Show(enter);
        if (connect) {
            Connect();
        }
    }

    void Connect() {
        h.Step();
        h.Accept();
        h.Step();
    }

    PredictionConfig Config(double scale = 1.0, double smoothing = 0.0) {
        PredictionConfig config;
        config.entity = entity;
        config.properties = {0};
        config.rule = MoveX(seen, scale);
        config.smoothing_seconds = smoothing;
        return config;
    }

    // The entity becomes visible to the client (batch 1) and the first state of tick 1 arrives, with x = `x`.
    void ShowEntity(double x) {
        h.SendBatch(++batches, ++tick, enter);
        source.MoveX(entity, static_cast<float>(x));
        h.SendStateBuffer(tick, batches, source.Write(tick), 0);
        h.Step();
    }

    // A state of the server for the next tick, with x = `x`, saying it reflects the inputs up to `sequence`.
    void Server(double x, uint32_t sequence) {
        source.MoveX(entity, static_cast<float>(x));
        ++tick;
        h.SendStateBuffer(tick, batches, source.Write(tick), sequence);
        h.Step();
    }

    void Input(uint8_t byte) {
        const uint8_t bytes[] = {byte};
        h.client.SendInput(bytes);
    }

    double X() const {
        double out[3] = {};
        TESSERA_CHECK(h.client.Predicted(out));
        return out[0];
    }
};

} // namespace

TESSERA_TEST(without_a_rule_nothing_changes) {
    Scene s;
    TESSERA_CHECK(s.h.client.Prediction() == PredictionStatus::Off);
    TESSERA_CHECK(s.h.client.TickRate() == 30);
    s.ShowEntity(5.0);
    double out[3] = {};
    TESSERA_CHECK(!s.h.client.Predicted(out) && s.h.client.PredictedComponents() == 0);
    // The inputs are numbered and sent as before.
    TESSERA_CHECK(s.h.client.SendInput(std::vector<uint8_t>{9}) == 1);
    TESSERA_CHECK(s.h.client.SendInput(std::vector<uint8_t>{8}) == 2);
    s.h.Step();
    const std::vector<Input> messages = s.h.Inputs();
    TESSERA_CHECK(messages.size() == 2 && messages[1].entries.size() == 2);
    TESSERA_CHECK(messages[1].entries.back().sequence == 2 && messages[1].entries.back().bytes[0] == 8);
    TESSERA_CHECK(s.h.client.Prediction() == PredictionStatus::Off && s.seen->calls == 0);
    TESSERA_CHECK(s.h.client.PredictionInfo().reconciliations == 0);
    TESSERA_CHECK(s.h.client.PredictionPending() == 0);
    // And a state buffer does what it always did.
    s.Server(6.0, 2);
    TESSERA_CHECK(s.h.client.Stats().buffers_applied == 2);
    TESSERA_CHECK(s.h.client.Prediction() == PredictionStatus::Off);
}

TESSERA_TEST(set_prediction_is_refused_for_an_unknown_type_or_property) {
    Scene s(false); // it works before connecting as well
    std::string error;
    PredictionConfig unknown_type = s.Config();
    unknown_type.entity = MakeEntity(kUnknownType, 1);
    TESSERA_CHECK(!s.h.client.SetPrediction(unknown_type, &error) && !error.empty());
    error.clear();
    PredictionConfig unknown_property = s.Config();
    unknown_property.properties = {17};
    TESSERA_CHECK(!s.h.client.SetPrediction(unknown_property, &error) && !error.empty());
    error.clear();
    PredictionConfig bit_mask = s.Config();
    bit_mask.properties = {3};
    TESSERA_CHECK(!s.h.client.SetPrediction(bit_mask, &error) && !error.empty());
    TESSERA_CHECK(!s.h.client.SetPrediction(unknown_type, nullptr));
    TESSERA_CHECK(s.h.client.Prediction() == PredictionStatus::Off);
    TESSERA_CHECK(s.h.client.SetPrediction(s.Config(), &error));
    TESSERA_CHECK(s.h.client.Prediction() == PredictionStatus::Waiting);
    s.h.client.ClearPrediction();
    TESSERA_CHECK(s.h.client.Prediction() == PredictionStatus::Off);
}

TESSERA_TEST(prediction_waits_for_the_first_state_of_the_entity) {
    Scene s;
    TESSERA_CHECK(s.h.client.SetPrediction(s.Config(), nullptr));
    TESSERA_CHECK(s.h.client.Prediction() == PredictionStatus::Waiting);
    s.Input(3); // before there is a state: not predicted
    s.Input(4);
    TESSERA_CHECK(s.seen->calls == 0 && s.h.client.PredictionPending() == 0);
    double out[3] = {};
    TESSERA_CHECK(!s.h.client.Predicted(out));
    s.ShowEntity(5.0);
    TESSERA_CHECK(s.h.client.Prediction() == PredictionStatus::Active);
    TESSERA_CHECK(s.h.client.PredictedComponents() == 3 && s.h.client.Predicted(out));
    TESSERA_CHECK(Near(out[0], 5.0) && s.h.client.PredictionPending() == 0);
    TESSERA_CHECK(s.seen->calls == 0); // the earlier inputs are not applied after the fact
}

TESSERA_TEST(send_input_predicts_at_once_with_one_step_of_one_over_tick_rate) {
    Scene s;
    TESSERA_CHECK(s.h.client.SetPrediction(s.Config(), nullptr));
    s.ShowEntity(5.0);
    s.Input(10); // no Poll or Step between the input and the read
    TESSERA_CHECK(s.seen->calls == 1 && s.seen->dts.size() == 1);
    TESSERA_CHECK(!s.seen->dts.empty() && std::fabs(s.seen->dts[0] - kTickDt) < 1e-12);
    TESSERA_CHECK(Near(s.X(), 15.0));
    s.Input(1);
    TESSERA_CHECK(Near(s.X(), 16.0) && s.h.client.PredictionPending() == 2);
}

TESSERA_TEST(a_state_buffer_confirming_the_inputs_up_to_n_replays_only_the_later_ones) {
    Scene s;
    TESSERA_CHECK(s.h.client.SetPrediction(s.Config(), nullptr));
    s.ShowEntity(5.0);
    for (uint8_t input = 1; input <= 4; ++input) {
        s.Input(input);
    }
    TESSERA_CHECK(Near(s.X(), 15.0));
    s.seen->calls = 0;
    s.Server(8.0, 2); // the server used inputs 1 and 2 and stands at 8
    TESSERA_CHECK(s.seen->calls == 2);          // only 3 and 4 were applied again
    TESSERA_CHECK(Near(s.X(), 8.0 + 3.0 + 4.0)); // and from the server's state
    TESSERA_CHECK(s.h.client.PredictionPending() == 2);
    TESSERA_CHECK(s.h.client.PredictionInfo().reconciliations == 2);
}

TESSERA_TEST(a_buffer_older_than_the_newest_reconciled_is_ignored_for_prediction) {
    Scene s;
    TESSERA_CHECK(s.h.client.SetPrediction(s.Config(), nullptr));
    s.ShowEntity(5.0);
    for (int i = 0; i < 6; ++i) {
        s.Input(1);
    }
    // Two states made in order, then delivered in the other order.
    s.source.MoveX(s.entity, 99.0f);
    const std::vector<uint8_t> at_nine = s.source.Write(9);
    s.source.MoveX(s.entity, 20.0f);
    const std::vector<uint8_t> at_ten = s.source.Write(10);
    s.h.SendStateBuffer(10, s.batches, at_ten, 5);
    s.h.Step();
    TESSERA_CHECK(Near(s.X(), 21.0) && s.h.client.PredictionPending() == 1);
    const uint64_t reconciled = s.h.client.PredictionInfo().reconciliations;
    s.h.SendStateBuffer(9, s.batches, at_nine, 6);
    s.h.Step();
    TESSERA_CHECK(s.h.client.Stats().buffers_applied == 3); // it was applied as history...
    // ...and the prediction did not go back.
    TESSERA_CHECK(Near(s.X(), 21.0) && s.h.client.PredictionPending() == 1);
    TESSERA_CHECK(s.h.client.PredictionInfo().reconciliations == reconciled);
    s.h.SendStateBuffer(10, s.batches, at_ten, 5); // a repeat
    s.h.Step();
    TESSERA_CHECK(Near(s.X(), 21.0) && s.h.client.PredictionInfo().reconciliations == reconciled);
}

TESSERA_TEST(a_held_buffer_keeps_its_confirmed_sequence_until_it_is_applied) {
    Scene s;
    TESSERA_CHECK(s.h.client.SetPrediction(s.Config(), nullptr));
    s.ShowEntity(5.0);
    for (uint8_t input = 1; input <= 4; ++input) {
        s.Input(input);
    }
    // The next buffer cites a second batch that has not arrived: it waits.
    s.source.MoveX(s.entity, 8.0f);
    s.h.SendStateBuffer(s.tick + 1, s.batches + 1, s.source.Write(s.tick + 1), 3);
    s.h.Step();
    TESSERA_CHECK(s.h.client.Stats().buffers_held == 1 && Near(s.X(), 15.0));
    s.h.SendBatch(s.batches + 1, s.tick + 1, {}); // the batch comes, and the buffer is applied
    s.h.Step();
    TESSERA_CHECK(s.h.client.Stats().buffers_applied == 2);
    TESSERA_CHECK(Near(s.X(), 8.0 + 4.0)); // inputs up to 3 were confirmed; only 4 is applied again
    TESSERA_CHECK(s.h.client.PredictionPending() == 1);
}

TESSERA_TEST(a_buffer_that_does_not_carry_the_entity_still_confirms_inputs) {
    Scene s;
    TESSERA_CHECK(s.h.client.SetPrediction(s.Config(), nullptr));
    s.ShowEntity(5.0);
    for (int i = 0; i < 3; ++i) {
        s.Input(1);
    }
    TESSERA_CHECK(Near(s.X(), 8.0) && s.h.client.PredictionPending() == 3);
    // Only the header and the end of the records: nothing changed for the entity, and the server says it has
    // used the inputs up to 2.
    s.h.SendStateBuffer(s.tick + 1, s.batches, std::vector<uint8_t>{0}, 2);
    s.h.Step();
    TESSERA_CHECK(s.h.client.Stats().buffers_applied == 2 && s.h.client.Stats().buffers_refused == 0);
    TESSERA_CHECK(s.h.client.PredictionPending() == 1);
    TESSERA_CHECK(Near(s.X(), 5.0 + 1.0)); // from the state it already had, and the one input left
    TESSERA_CHECK(s.h.client.PredictionInfo().reconciliations == 2);
}

TESSERA_TEST(prediction_state_is_discarded_on_disconnect_rejection_and_protocol_error) {
    {
        Scene s;
        TESSERA_CHECK(s.h.client.SetPrediction(s.Config(), nullptr));
        s.ShowEntity(5.0);
        s.Input(1);
        TESSERA_CHECK(s.h.client.Prediction() == PredictionStatus::Active);
        s.h.network.Server().Disconnect(s.h.connection);
        s.h.Step();
        s.h.Step();
        TESSERA_CHECK(s.h.client.State() == ClientState::Disconnected);
        double out[3] = {};
        TESSERA_CHECK(!s.h.client.Predicted(out) && s.h.client.PredictionPending() == 0);
        TESSERA_CHECK(s.h.client.Prediction() == PredictionStatus::Waiting); // the configuration stays
    }
    {
        Scene s(false);
        TESSERA_CHECK(s.h.client.SetPrediction(s.Config(), nullptr));
        s.h.Step();
        s.h.Reject(RejectReason::ServerFull);
        s.h.Step();
        TESSERA_CHECK(s.h.client.State() == ClientState::Rejected);
        double out[3] = {};
        TESSERA_CHECK(!s.h.client.Predicted(out) && s.h.client.Prediction() == PredictionStatus::Waiting);
    }
    {
        Scene s;
        TESSERA_CHECK(s.h.client.SetPrediction(s.Config(), nullptr));
        s.ShowEntity(5.0);
        s.Input(1);
        s.h.Send(Channel::ReliableOrdered, std::vector<uint8_t>{0xFF, 0xFF, 0xFF});
        s.h.Step();
        TESSERA_CHECK(s.h.client.State() == ClientState::Disconnected);
        TESSERA_CHECK(s.h.client.Stats().protocol_errors == 1);
        double out[3] = {};
        TESSERA_CHECK(!s.h.client.Predicted(out) && s.h.client.PredictionPending() == 0);
        TESSERA_CHECK(s.h.client.Prediction() == PredictionStatus::Waiting);
    }
}

TESSERA_TEST(the_entity_leaving_discards_the_prediction_and_it_resumes_when_it_returns) {
    Scene s;
    TESSERA_CHECK(s.h.client.SetPrediction(s.Config(), nullptr));
    s.ShowEntity(5.0);
    s.Input(1);
    s.Input(1);
    TESSERA_CHECK(s.h.client.Prediction() == PredictionStatus::Active && s.h.client.PredictionPending() == 2);
    // It goes out of view.
    s.h.SendBatch(++s.batches, ++s.tick, {Leave(s.entity, 0)});
    s.h.Step();
    TESSERA_CHECK(s.h.client.WordsOf(s.entity) == nullptr);
    double out[3] = {};
    TESSERA_CHECK(!s.h.client.Predicted(out));
    TESSERA_CHECK(s.h.client.Prediction() == PredictionStatus::Waiting);
    TESSERA_CHECK(s.h.client.PredictionPending() == 0);
    const int calls = s.seen->calls;
    s.Input(1); // not predicted while there is nothing to predict from
    TESSERA_CHECK(s.seen->calls == calls);
    // It comes back, and the next state of it starts the prediction again.
    s.source.Show({Leave(s.entity, 0)});
    s.source.Show(s.enter);
    s.h.SendBatch(++s.batches, ++s.tick, s.enter);
    s.source.MoveX(s.entity, 40.0f);
    ++s.tick;
    s.h.SendStateBuffer(s.tick, s.batches, s.source.Write(s.tick), 0);
    s.h.Step();
    TESSERA_CHECK(s.h.client.Prediction() == PredictionStatus::Active);
    TESSERA_CHECK(s.h.client.Predicted(out) && Near(out[0], 40.0));
}

TESSERA_TEST(a_rule_that_throws_is_caught_and_disables_prediction_but_not_the_client) {
    Scene s;
    PredictionConfig config = s.Config();
    config.rule = [](std::span<double>, std::span<const uint8_t>, double) -> bool {
        throw std::runtime_error("the rule broke");
    };
    TESSERA_CHECK(s.h.client.SetPrediction(config, nullptr));
    s.ShowEntity(5.0);
    bool escaped = false;
    try {
        TESSERA_CHECK(s.h.client.SendInput(std::vector<uint8_t>{1}) == 1); // the input itself still goes
    } catch (...) {
        escaped = true;
    }
    TESSERA_CHECK(!escaped);
    TESSERA_CHECK(s.h.client.Prediction() == PredictionStatus::Failed);
    TESSERA_CHECK(s.h.client.PredictionInfo().rule_failures == 1);
    double out[3] = {};
    TESSERA_CHECK(!s.h.client.Predicted(out));
    // The client carries on: it applies state, confirms it and sends the next input.
    s.Server(6.0, 1);
    TESSERA_CHECK(s.h.client.State() == ClientState::Connected && s.h.client.Stats().buffers_applied == 2);
    TESSERA_CHECK(s.h.client.SendInput(std::vector<uint8_t>{1}) == 2);
    TESSERA_CHECK(s.h.client.PredictionInfo().rule_failures == 1 && s.h.client.Stats().protocol_errors == 0);
    s.h.Step();
    TESSERA_CHECK(s.h.Inputs().size() == 2 && !s.h.Acks().empty());
}

TESSERA_TEST(two_clients_predict_independently) {
    Scene a;
    Scene b;
    TESSERA_CHECK(a.h.client.SetPrediction(a.Config(1.0), nullptr));
    TESSERA_CHECK(b.h.client.SetPrediction(b.Config(2.0), nullptr));
    a.ShowEntity(5.0);
    b.ShowEntity(50.0);
    a.Input(3);
    b.Input(3);
    TESSERA_CHECK(Near(a.X(), 8.0) && Near(b.X(), 56.0));
    a.Server(10.0, 0);
    TESSERA_CHECK(Near(a.X(), 13.0) && Near(b.X(), 56.0));
    TESSERA_CHECK(a.seen->calls == 2 && b.seen->calls == 1);
    TESSERA_CHECK(a.h.client.PredictionPending() == 1 && b.h.client.PredictionPending() == 1);
}

TESSERA_TEST(advance_prediction_moves_the_slide_by_the_time_given) {
    Scene s;
    TESSERA_CHECK(s.h.client.SetPrediction(s.Config(0.5, 0.1), nullptr));
    s.ShowEntity(100.0);
    s.Input(2); // this client moves half of the input: 101
    TESSERA_CHECK(Near(s.X(), 101.0));
    s.Server(102.0, 1); // the server moved it by the whole input
    TESSERA_CHECK(Near(s.h.client.PredictionInfo().last_error, 1.0, 0.01));
    TESSERA_CHECK(Near(s.X(), 101.0, 0.01)); // what is shown does not jump
    s.h.client.AdvancePrediction(0.05);
    TESSERA_CHECK(Near(s.X(), 101.5, 0.01));
    s.h.client.AdvancePrediction(0.05);
    TESSERA_CHECK(Near(s.X(), 102.0, 0.01));
    s.h.client.AdvancePrediction(-3.0); // time going backwards changes nothing
    TESSERA_CHECK(Near(s.X(), 102.0, 0.01));
}

TESSERA_TEST(an_input_refused_for_its_size_is_not_predicted) {
    Scene s;
    TESSERA_CHECK(s.h.client.SetPrediction(s.Config(), nullptr));
    s.ShowEntity(5.0);
    const int calls = s.seen->calls;
    TESSERA_CHECK(s.h.client.SendInput(std::vector<uint8_t>(17, 1)) == 0);
    TESSERA_CHECK(s.h.client.Stats().inputs_refused == 1);
    TESSERA_CHECK(s.seen->calls == calls && s.h.client.PredictionPending() == 0);
    TESSERA_CHECK(Near(s.X(), 5.0));
    // A 16-byte input is the largest and is predicted.
    TESSERA_CHECK(s.h.client.SendInput(std::vector<uint8_t>(16, 1)) == 1);
    TESSERA_CHECK(s.seen->calls == calls + 1 && Near(s.X(), 6.0));
}
