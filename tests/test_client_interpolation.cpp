// SPDX-License-Identifier: Apache-2.0
// The client's interpolation: how it is fed by the states the client applies, how the entities coming and
// going and the connection ending reach it, and how it sits next to the prediction.
#include "interpolation_test_support.hpp"
#include "network_test_support.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace network_test;
using namespace interpolation_test;

namespace {

bool Near(double a, double b, double tolerance = kStep) {
    return std::fabs(a - b) <= tolerance;
}

// A client that is connected (tick rate 30), a source of real state buffers, and some entities of the test
// mob type under the compact ids 0, 1, 2...
struct Scene {
    ClientHarness h;
    ReplicationSource source{h.schema};
    std::vector<EntityId> entities;
    std::vector<VisibilityEvent> enter;
    uint64_t batches = 0;
    uint64_t tick = 0;

    // `show_all` makes every entity visible to the source at once; a test that wants to say which are
    // visible, and as what, passes false and calls `source.Show` itself.
    explicit Scene(uint32_t count = 1, bool connect = true, bool show_all = true) {
        for (uint32_t i = 0; i < count; ++i) {
            entities.push_back(source.Spawn(10.0f + static_cast<float>(i), 0.0f, 0.0f));
            enter.push_back(Enter(entities.back(), i));
        }
        if (show_all) {
            source.Show(enter);
        }
        if (connect) {
            Connect();
        }
    }

    void Connect() {
        h.Step();
        h.Accept();
        h.Step();
    }

    // The entities become visible (batch 1) and the first states, of tick 1, arrive.
    void Show() {
        h.SendBatch(++batches, ++tick, enter);
        h.SendStateBuffer(tick, batches, source.Write(tick), 0);
        h.Step();
    }

    // Moves entity `index` to `x` and writes the buffer of the next tick, without delivering it.
    std::vector<uint8_t> Write(uint32_t index, double x) {
        source.MoveX(entities[index], static_cast<float>(x));
        return source.Write(++tick);
    }

    void Deliver(uint64_t buffer_tick, const std::vector<uint8_t>& payload, uint32_t sequence = 0) {
        h.SendStateBuffer(buffer_tick, batches, payload, sequence);
        h.Step();
    }

    // Moves entity `index` to `x` and delivers the buffer of the next tick.
    void Move(uint32_t index, double x, uint32_t sequence = 0) {
        const std::vector<uint8_t> payload = Write(index, x);
        Deliver(tick, payload, sequence);
    }

    double X(uint32_t index) const {
        return XOf(h.schema, h.client.WordsOf(entities[index]));
    }
};

// Collects what a `ReceiverState` tells its observer.
struct Collector : IRecordObserver {
    struct Call {
        uint32_t id;
        uint64_t tick;
        std::vector<uint32_t> words;
    };
    uint32_t words_per_state = 9;
    std::vector<Call> calls;

    void OnRecord(uint32_t compact_id, uint64_t tick, const uint32_t* words) override {
        calls.push_back(Call{compact_id, tick, std::vector<uint32_t>(words, words + words_per_state)});
    }
};

} // namespace

TESSERA_TEST(without_a_config_nothing_changes) {
    Scene s;
    s.Show();
    for (int i = 0; i < 3; ++i) {
        s.Move(0, 11.0 + i);
    }
    ClientReplica& c = s.h.client;
    TESSERA_CHECK(!c.InterpolationSet() && !c.Interpolating());
    TESSERA_CHECK(c.DisplayedWords(s.entities[0]) == nullptr);
    double out[3] = {};
    TESSERA_CHECK(!c.DisplayedValue(s.entities[0], 0, out));
    TESSERA_CHECK(c.DisplayedTick() == 0.0 && c.InterpolationLag() == 0.0);
    const InterpolationStats& stats = c.InterpolationInfo();
    TESSERA_CHECK(stats.states == 0 && stats.states_ignored == 0 && stats.gap_holds == 0 &&
                  stats.snaps == 0 && stats.stalls == 0 && stats.resyncs == 0);
    TESSERA_CHECK(c.InterpolationMemoryBytes() == 0);
    c.AdvanceInterpolation(1.0);
    TESSERA_CHECK(c.DisplayedTick() == 0.0);
    // What the client holds is what it always held.
    TESSERA_CHECK(SameWords(c.WordsOf(s.entities[0]), s.source.Words(s.entities[0])));
    TESSERA_CHECK(c.StateTickOf(s.entities[0]) == 4);
    TESSERA_CHECK(c.Stats().buffers_applied == 4);
}

TESSERA_TEST(set_interpolation_is_refused_for_bad_values_and_changes_nothing) {
    Scene s;
    ClientReplica& c = s.h.client;
    std::string error;
    InterpolationConfig bad;
    bad.delay_ticks = 31;
    TESSERA_CHECK(!c.SetInterpolation(bad, &error) && !error.empty());
    TESSERA_CHECK(!c.InterpolationSet());

    InterpolationConfig good;
    good.delay_ticks = 4;
    good.snap_distance = 3.0;
    TESSERA_CHECK(c.SetInterpolation(good, &error));
    TESSERA_CHECK(c.InterpolationSet());
    for (double snap : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                        std::numeric_limits<double>::infinity()}) {
        InterpolationConfig config;
        config.snap_distance = snap;
        error.clear();
        TESSERA_CHECK(!c.SetInterpolation(config, &error) && !error.empty());
    }
    InterpolationConfig no_gap;
    no_gap.max_gap_ticks = 0;
    TESSERA_CHECK(!c.SetInterpolation(no_gap, &error));
    TESSERA_CHECK(!c.SetInterpolation(bad, nullptr));
    TESSERA_CHECK(c.InterpolationSet());
    TESSERA_CHECK(c.InterpolationSettings().delay_ticks == 4);
    TESSERA_CHECK(c.InterpolationSettings().snap_distance == 3.0);
}

TESSERA_TEST(the_interpolator_gets_the_same_states_as_the_receiver) {
    Scene s(3);
    InterpolationConfig config;
    TESSERA_CHECK(s.h.client.SetInterpolation(config));
    s.Show();
    std::mt19937 random(7);
    struct Pending {
        uint64_t order;
        uint64_t tick;
        std::vector<uint8_t> payload;
    };
    std::vector<Pending> pending;
    for (int i = 0; i < 200; ++i) {
        const double x = 12.0 + 0.3 * i;
        s.source.MoveX(s.entities[0], static_cast<float>(x));
        s.source.MoveX(s.entities[1], static_cast<float>(x + 1.0));
        s.source.MoveX(s.entities[2], static_cast<float>(x + 2.0));
        std::vector<uint8_t> payload = s.source.Write(++s.tick);
        if (std::uniform_real_distribution<double>(0.0, 1.0)(random) < 0.3) {
            continue; // lost
        }
        pending.push_back(Pending{s.tick + static_cast<uint64_t>(random() % 4), s.tick, std::move(payload)});
    }
    std::stable_sort(pending.begin(), pending.end(),
                     [](const Pending& a, const Pending& b) { return a.order < b.order; });
    for (const Pending& buffer : pending) {
        s.Deliver(buffer.tick, buffer.payload);
    }
    ClientReplica& c = s.h.client;
    c.AdvanceInterpolation(100.0); // far past the newest state: the clock stops at it
    for (uint32_t i = 0; i < 3; ++i) {
        const EntityId e = s.entities[i];
        const size_t count = c.InterpolationStateCount(e);
        TESSERA_CHECK(count > 0 && count <= 8);
        TESSERA_CHECK(c.InterpolationStateTick(e, static_cast<uint32_t>(count - 1)) == c.StateTickOf(e));
        TESSERA_CHECK(SameWords(c.DisplayedWords(e), c.WordsOf(e)));
        for (size_t k = 1; k < count; ++k) {
            TESSERA_CHECK(c.InterpolationStateTick(e, static_cast<uint32_t>(k)) >
                          c.InterpolationStateTick(e, static_cast<uint32_t>(k - 1)));
        }
    }
}

TESSERA_TEST(a_late_buffer_is_kept_as_history_for_interpolation_but_not_as_newest) {
    Scene s;
    InterpolationConfig config;
    TESSERA_CHECK(s.h.client.SetInterpolation(config));
    s.Show(); // tick 1
    const std::vector<uint8_t> second = s.Write(0, 12.0);
    const std::vector<uint8_t> third = s.Write(0, 13.0);
    const std::vector<uint8_t> fourth = s.Write(0, 14.0);
    s.Deliver(2, second);
    s.Deliver(4, fourth);
    s.Deliver(3, third); // late
    ClientReplica& c = s.h.client;
    TESSERA_CHECK(c.StateTickOf(s.entities[0]) == 4);
    TESSERA_CHECK(Near(s.X(0), 14.0));
    TESSERA_CHECK(c.InterpolationStateCount(s.entities[0]) == 4);
    for (uint32_t i = 0; i < 4; ++i) {
        TESSERA_CHECK(c.InterpolationStateTick(s.entities[0], i) == i + 1);
    }
    TESSERA_CHECK(c.Stats().buffers_applied == 4);
}

TESSERA_TEST(a_buffer_with_no_record_for_the_entity_means_it_did_not_change) {
    Scene s(2);
    InterpolationConfig config;
    TESSERA_CHECK(s.h.client.SetInterpolation(config));
    s.Show();
    s.source.engine.Confirm(0, s.tick); // the server now writes only what changed
    for (int i = 0; i < 6; ++i) {
        s.Move(0, 12.0 + i); // only the first entity moves
        s.source.engine.Confirm(0, s.tick);
    }
    ClientReplica& c = s.h.client;
    TESSERA_CHECK(c.InterpolationStateCount(s.entities[1]) == 1);
    TESSERA_CHECK(c.InterpolationStateCount(s.entities[0]) == 7);
    c.AdvanceInterpolation(1.0);
    TESSERA_CHECK(c.DisplayedTick() == 7.0); // the clock follows the buffers, not the entity's records
    TESSERA_CHECK(SameWords(c.DisplayedWords(s.entities[1]), c.WordsOf(s.entities[1])));
    double out[3] = {};
    TESSERA_CHECK(c.DisplayedValue(s.entities[1], 0, out));
    TESSERA_CHECK(Near(out[0], 11.0));
}

TESSERA_TEST(a_buffer_the_receiver_refuses_does_not_move_the_clock) {
    Scene s(2, false, false);
    s.source.Show({Enter(s.entities[0], 0), Enter(s.entities[1], 7)}); // the engine knows a compact id 7
    s.Connect();
    InterpolationConfig config;
    TESSERA_CHECK(s.h.client.SetInterpolation(config));
    s.h.SendBatch(++s.batches, ++s.tick, {Enter(s.entities[0], 0)}); // the client was told only of id 0
    s.h.Step();
    // This buffer has a record for id 7, which the client does not know: it is refused.
    const std::vector<uint8_t> refused = s.Write(0, 11.0);
    s.Deliver(s.tick, refused);
    ClientReplica& c = s.h.client;
    TESSERA_CHECK(c.Stats().buffers_refused == 1 && c.Stats().buffers_applied == 0);
    TESSERA_CHECK(!c.Interpolating());
    TESSERA_CHECK(c.DisplayedTick() == 0.0);
    TESSERA_CHECK(c.InterpolationInfo().states == 1); // the record before the bad one was kept
}

TESSERA_TEST(a_held_buffer_feeds_the_interpolator_when_it_is_applied) {
    Scene s(2, false, false);
    s.source.Show({s.enter[0]}); // only the first is visible to begin with
    s.Connect();
    InterpolationConfig config;
    TESSERA_CHECK(s.h.client.SetInterpolation(config));
    s.h.SendBatch(++s.batches, ++s.tick, {s.enter[0]});
    s.h.SendStateBuffer(s.tick, s.batches, s.source.Write(s.tick), 0);
    s.h.Step();
    s.source.Show({s.enter[1]});
    const std::vector<uint8_t> second = s.Write(0, 12.0); // tick 2: both entities have a record
    s.h.SendStateBuffer(2, 2, second, 0); // it cites the second batch, which has not arrived
    s.h.Step();
    ClientReplica& c = s.h.client;
    TESSERA_CHECK(c.Stats().buffers_held == 1);
    TESSERA_CHECK(c.InterpolationStateCount(s.entities[0]) == 1);
    s.h.SendBatch(++s.batches, 2, {s.enter[1]}); // the events arrive: the buffer is applied
    s.h.Step();
    TESSERA_CHECK(c.Stats().buffers_applied == 2);
    TESSERA_CHECK(c.InterpolationStateCount(s.entities[0]) == 2);
    TESSERA_CHECK(c.InterpolationStateCount(s.entities[1]) == 1);
    TESSERA_CHECK(c.InterpolationStateTick(s.entities[1], 0) == 2);
    TESSERA_CHECK(c.StateTickOf(s.entities[1]) == 2);
}

TESSERA_TEST(a_new_entity_appears_at_its_first_state) {
    Scene s(2, false, false);
    s.source.Show({s.enter[0]});
    s.Connect();
    InterpolationConfig config;
    TESSERA_CHECK(s.h.client.SetInterpolation(config));
    s.h.SendBatch(++s.batches, ++s.tick, {s.enter[0]});
    s.h.SendStateBuffer(s.tick, s.batches, s.source.Write(s.tick), 0);
    s.h.Step();
    for (int i = 0; i < 4; ++i) {
        s.Move(0, 12.0 + i);
    }
    // The second entity comes into view, far from where anything has been.
    s.source.MoveX(s.entities[1], 55.0f);
    s.source.Show({s.enter[1]});
    s.h.SendBatch(++s.batches, s.tick + 1, {s.enter[1]});
    const std::vector<uint8_t> first = s.source.Write(++s.tick);
    s.Deliver(s.tick, first);
    ClientReplica& c = s.h.client;
    double out[3] = {};
    for (int step = 0; step < 20; ++step) {
        c.AdvanceInterpolation(0.01);
        if (const uint32_t* words = c.DisplayedWords(s.entities[1])) {
            TESSERA_CHECK(Near(XOf(s.h.schema, words), 55.0));
            TESSERA_CHECK(c.DisplayedValue(s.entities[1], 0, out) && Near(out[0], 55.0));
        }
    }
    TESSERA_CHECK(c.DisplayedWords(s.entities[1]) != nullptr);
    TESSERA_CHECK(c.InterpolationStateCount(s.entities[1]) == 1);
}

TESSERA_TEST(an_entity_leaving_loses_its_history_and_a_reused_id_never_blends_two_entities) {
    Scene s;
    InterpolationConfig config;
    TESSERA_CHECK(s.h.client.SetInterpolation(config));
    s.Show();
    for (int i = 0; i < 3; ++i) {
        s.Move(0, 12.0 + i);
    }
    ClientReplica& c = s.h.client;
    c.AdvanceInterpolation(0.05); // in the middle of a segment of the first entity
    const EntityId old_entity = s.entities[0];
    TESSERA_CHECK(c.DisplayedWords(old_entity) != nullptr);

    // The first entity leaves and a second takes its compact id, far away.
    const EntityId other = s.source.Spawn(80.0f, 0.0f, 0.0f);
    const std::vector<VisibilityEvent> swap = {Leave(old_entity, 0), Enter(other, 0)};
    s.source.Show(swap);
    s.h.SendBatch(++s.batches, s.tick + 1, swap);
    const std::vector<uint8_t> arrival = s.source.Write(++s.tick);
    s.Deliver(s.tick, arrival);
    TESSERA_CHECK(c.DisplayedWords(old_entity) == nullptr);
    TESSERA_CHECK(c.InterpolationStateCount(old_entity) == 0);
    TESSERA_CHECK(c.InterpolationStateCount(other) == 1);
    double out[3] = {};
    for (int step = 0; step < 50; ++step) {
        c.AdvanceInterpolation(0.01);
        TESSERA_CHECK(c.DisplayedValue(other, 0, out));
        TESSERA_CHECK(Near(out[0], 80.0)); // never on its way from the first entity's place
    }
    const uint32_t* shown_words = c.DisplayedWords(other);
    TESSERA_CHECK(shown_words != nullptr && Near(XOf(s.h.schema, shown_words), 80.0));
    TESSERA_CHECK(c.InterpolationEntities() == 1); // the first entity's room was given back

    // An entity that becomes visible under another compact id leaves the old one behind.
    s.h.SendBatch(++s.batches, s.tick + 1, {Enter(other, 3)});
    s.h.Step();
    TESSERA_CHECK(c.InterpolationEntities() == 1);
    TESSERA_CHECK(c.InterpolationStateCount(other) == 0);
    // And one that leaves, with nothing taking its place, gives its room back.
    s.h.SendBatch(++s.batches, s.tick + 1, {Leave(other, 3)});
    s.h.Step();
    TESSERA_CHECK(c.InterpolationEntities() == 0);
}

TESSERA_TEST(a_disconnect_a_rejection_and_a_protocol_error_clear_the_history_and_keep_the_config) {
    InterpolationConfig config;
    config.delay_ticks = 3;
    // A disconnect.
    {
        Scene s;
        TESSERA_CHECK(s.h.client.SetInterpolation(config));
        s.Show();
        s.Move(0, 12.0);
        s.h.client.AdvanceInterpolation(0.1);
        TESSERA_CHECK(s.h.client.Interpolating() && s.h.client.DisplayedWords(s.entities[0]) != nullptr);
        s.h.client.Disconnect();
        TESSERA_CHECK(!s.h.client.Interpolating());
        TESSERA_CHECK(s.h.client.DisplayedWords(s.entities[0]) == nullptr);
        TESSERA_CHECK(s.h.client.InterpolationSet());
        TESSERA_CHECK(s.h.client.InterpolationSettings().delay_ticks == 3);
    }
    // A rejection (nothing was shown yet).
    {
        Scene s(1, false);
        TESSERA_CHECK(s.h.client.SetInterpolation(config));
        s.h.Step();
        s.h.Reject(RejectReason::SchemaMismatch);
        s.h.Step();
        TESSERA_CHECK(s.h.client.State() == ClientState::Rejected);
        TESSERA_CHECK(!s.h.client.Interpolating() && s.h.client.InterpolationSet());
    }
    // The server breaks the protocol.
    {
        Scene s;
        TESSERA_CHECK(s.h.client.SetInterpolation(config));
        s.Show();
        s.Move(0, 12.0);
        TESSERA_CHECK(s.h.client.Interpolating());
        s.h.Send(Channel::ReliableOrdered, std::vector<uint8_t>{0xFF, 0xFF, 0xFF});
        s.h.Step();
        TESSERA_CHECK(s.h.client.State() == ClientState::Disconnected);
        TESSERA_CHECK(!s.h.client.Interpolating());
        TESSERA_CHECK(s.h.client.DisplayedWords(s.entities[0]) == nullptr);
        TESSERA_CHECK(s.h.client.InterpolationSet());
        TESSERA_CHECK(s.h.client.InterpolationSettings().delay_ticks == 3);
    }
}

TESSERA_TEST(enabling_while_connected_seeds_the_states_already_held) {
    Scene s(2);
    s.Show();
    for (int i = 0; i < 3; ++i) {
        s.Move(0, 12.0 + i);
    }
    ClientReplica& c = s.h.client;
    TESSERA_CHECK(!c.InterpolationSet());
    InterpolationConfig config;
    TESSERA_CHECK(c.SetInterpolation(config));
    TESSERA_CHECK(c.Interpolating());
    TESSERA_CHECK(c.InterpolationStateCount(s.entities[0]) == 1);
    TESSERA_CHECK(c.InterpolationStateCount(s.entities[1]) == 1);
    TESSERA_CHECK(c.DisplayedTick() == 2.0); // the newest tick (4) minus the delay
    c.AdvanceInterpolation(1.0 / 30.0);
    TESSERA_CHECK(Near(c.DisplayedTick(), 3.0, 1e-9));
    // The newest state is in the past of the shown time: that is what is shown.
    TESSERA_CHECK(SameWords(c.DisplayedWords(s.entities[0]), c.WordsOf(s.entities[0])));
    double out[3] = {};
    TESSERA_CHECK(c.DisplayedValue(s.entities[0], 0, out) && Near(out[0], 14.0));
    // Enabling it again starts over from what is held.
    TESSERA_CHECK(c.SetInterpolation(config));
    TESSERA_CHECK(c.DisplayedTick() == 2.0);
}

TESSERA_TEST(advance_before_the_hello_is_harmless) {
    Scene s(1, false);
    ClientReplica& c = s.h.client;
    InterpolationConfig config;
    TESSERA_CHECK(c.SetInterpolation(config)); // before anything is connected
    TESSERA_CHECK(c.TickRate() == 0);
    c.AdvanceInterpolation(1.0);
    c.AdvanceInterpolation(0.0);
    TESSERA_CHECK(!c.Interpolating());
    TESSERA_CHECK(c.DisplayedTick() == 0.0);
    TESSERA_CHECK(c.DisplayedWords(s.entities[0]) == nullptr);
    s.Connect();
    s.Show();
    TESSERA_CHECK(c.Interpolating()); // the first buffer starts the clock
}

TESSERA_TEST(advance_interpolation_moves_the_display_by_the_time_given_at_the_tick_rate) {
    Scene s;
    s.Show();
    for (int i = 0; i < 19; ++i) {
        s.Move(0, 11.0 + i);
    }
    ClientReplica& c = s.h.client;
    InterpolationConfig config;
    TESSERA_CHECK(c.SetInterpolation(config));
    TESSERA_CHECK(c.DisplayedTick() == 18.0);
    c.AdvanceInterpolation(1.0 / 30.0);
    TESSERA_CHECK(Near(c.DisplayedTick(), 19.0, 1e-9)); // one tick at 30 ticks a second
    c.AdvanceInterpolation(0.5 / 30.0);
    TESSERA_CHECK(Near(c.DisplayedTick(), 19.5, 1e-9));
    const double shown = c.DisplayedTick();
    for (double bad : {-1.0, 0.0, std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity()}) {
        c.AdvanceInterpolation(bad);
        TESSERA_CHECK(c.DisplayedTick() == shown);
    }
    TESSERA_CHECK(Near(c.InterpolationLag(), 0.5, 1e-9));
}

TESSERA_TEST(two_clients_interpolate_independently) {
    Scene a;
    Scene b;
    a.Show();
    b.Show();
    for (int i = 0; i < 9; ++i) {
        a.Move(0, 12.0 + i);
        b.Move(0, 12.0 + i);
    }
    InterpolationConfig two;
    InterpolationConfig six;
    six.delay_ticks = 6;
    TESSERA_CHECK(a.h.client.SetInterpolation(two));
    TESSERA_CHECK(!b.h.client.InterpolationSet() && !b.h.client.Interpolating());
    TESSERA_CHECK(b.h.client.DisplayedWords(b.entities[0]) == nullptr);
    TESSERA_CHECK(b.h.client.SetInterpolation(six));
    TESSERA_CHECK(a.h.client.DisplayedTick() == 8.0 && b.h.client.DisplayedTick() == 4.0);
    a.h.client.AdvanceInterpolation(0.1);
    TESSERA_CHECK(b.h.client.DisplayedTick() == 4.0);
    TESSERA_CHECK(a.h.client.InterpolationSettings().delay_ticks == 2);
    TESSERA_CHECK(b.h.client.InterpolationSettings().delay_ticks == 6);
    // Another entity moves in one of them only.
    a.Move(0, 30.0);
    TESSERA_CHECK(a.h.client.InterpolationInfo().states > b.h.client.InterpolationInfo().states);
}

TESSERA_TEST(interpolation_and_prediction_work_together_without_disturbing_each_other) {
    Scene s(2);
    ClientReplica& c = s.h.client;
    // The rule notes what the interpolation looked like when it ran inside a state buffer being applied.
    struct Seen {
        bool in_poll = false;
        std::vector<double> lags;
        std::vector<bool> running;
    };
    auto seen = std::make_shared<Seen>();
    PredictionConfig prediction;
    prediction.entity = s.entities[0];
    prediction.properties = {0};
    prediction.smoothing_seconds = 0.0;
    prediction.rule = [seen, client = &c](std::span<double> state, std::span<const uint8_t> input, double) {
        if (seen->in_poll) {
            seen->lags.push_back(client->InterpolationLag());
            seen->running.push_back(client->Interpolating());
        }
        state[0] += input.empty() ? 0.0 : static_cast<double>(input[0]);
        return true;
    };
    TESSERA_CHECK(c.SetPrediction(prediction));
    InterpolationConfig config;
    TESSERA_CHECK(c.SetInterpolation(config));
    s.Show(); // tick 1
    const uint8_t input[] = {1};
    TESSERA_CHECK(c.SendInput(input) == 1);
    seen->in_poll = true;
    s.Move(1, 40.0); // tick 2, which does not yet reflect the input
    seen->in_poll = false;

    // The prediction replayed the input over the state of the server: 10 + 1.
    double predicted[3] = {};
    TESSERA_CHECK(c.Prediction() == PredictionStatus::Active && c.Predicted(predicted));
    TESSERA_CHECK(Near(predicted[0], 11.0));
    // The interpolation had already been told of the buffer (its newest tick is 2) when the rule ran.
    TESSERA_CHECK(!seen->lags.empty());
    TESSERA_CHECK(seen->lags.front() == 2.0 && seen->running.front());
    // And the other entity is shown from what the interpolator holds, untouched by the prediction.
    double shown[3] = {};
    c.AdvanceInterpolation(0.2);
    TESSERA_CHECK(c.DisplayedValue(s.entities[1], 0, shown) && Near(shown[0], 40.0));
    TESSERA_CHECK(c.InterpolationInfo().states >= 4);
    TESSERA_CHECK(c.PredictionInfo().reconciliations == 2);
}

TESSERA_TEST(the_observer_is_called_for_each_applied_record_in_order_with_the_stored_words) {
    Scene s(2, false);
    ReceiverState receiver(s.h.schema, 8, 32);
    TESSERA_CHECK(receiver.OnEnter(0, s.entities[0], 1) && receiver.OnEnter(1, s.entities[1], 1));
    receiver.AdvanceEvents(10);
    Collector collector;
    for (uint64_t tick = 2; tick <= 4; ++tick) {
        s.source.MoveX(s.entities[0], static_cast<float>(10 + tick));
        s.source.MoveX(s.entities[1], static_cast<float>(20 + tick));
        s.tick = tick;
        const std::vector<uint8_t> payload = s.source.Write(tick);
        size_t applied = 0;
        TESSERA_CHECK(receiver.Apply(payload, tick, &applied, &collector) == DecodeStatus::Ok);
        TESSERA_CHECK(applied == 2);
    }
    TESSERA_CHECK(collector.calls.size() == 6);
    for (size_t i = 0; i < collector.calls.size(); ++i) {
        const Collector::Call& call = collector.calls[i];
        TESSERA_CHECK(call.id == i % 2); // the lower compact id first, as they are written
        TESSERA_CHECK(call.tick == 2 + i / 2);
        TESSERA_CHECK(call.words.size() == 9);
    }
    // The last call of each entity carries exactly the state the receiver keeps.
    TESSERA_CHECK(SameWords(collector.calls[4].words.data(), receiver.Current(0)));
    TESSERA_CHECK(SameWords(collector.calls[5].words.data(), receiver.Current(1)));

    // Without an observer the same thing happens.
    ReceiverState plain(s.h.schema, 8, 32);
    plain.OnEnter(0, s.entities[0], 1);
    plain.OnEnter(1, s.entities[1], 1);
    plain.AdvanceEvents(10);
    s.source.MoveX(s.entities[0], 50.0f);
    s.tick = 5;
    const std::vector<uint8_t> payload = s.source.Write(5);
    TESSERA_CHECK(plain.Apply(payload, 5) == DecodeStatus::Ok);
    TESSERA_CHECK(plain.CurrentTick(0) == 5);
}

TESSERA_TEST(a_refused_record_is_not_reported_to_the_observer) {
    Scene s(2, false, false);
    s.source.Show({Enter(s.entities[0], 0), Enter(s.entities[1], 5)});
    ReceiverState receiver(s.h.schema, 8, 32);
    TESSERA_CHECK(receiver.OnEnter(0, s.entities[0], 1)); // it was never told of compact id 5
    receiver.AdvanceEvents(10);
    s.source.MoveX(s.entities[0], 12.0f);
    s.source.MoveX(s.entities[1], 22.0f);
    const std::vector<uint8_t> payload = s.source.Write(2);
    Collector collector;
    size_t applied = 0;
    TESSERA_CHECK(receiver.Apply(payload, 2, &applied, &collector) == DecodeStatus::UnknownEntity);
    TESSERA_CHECK(applied == 1);
    TESSERA_CHECK(collector.calls.size() == 1);
    TESSERA_CHECK(collector.calls[0].id == 0 && collector.calls[0].tick == 2);
    TESSERA_CHECK(receiver.Current(5) == nullptr);
}
