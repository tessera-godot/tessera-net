// SPDX-License-Identifier: Apache-2.0
// Prediction against the example server on a simulated network: the player's client moves its own sphere with
// the same movement rule as the server, and each state of the server corrects it.
#include "example_game.hpp"
#include "tessera/net/client_replica.hpp"
#include "tessera/net/replication/property_codecs.hpp"
#include "tessera/net/transport/simulated_network.hpp"
#include "tessera/server/server_host.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace tessera::net;
using namespace tessera::server;
using tessera::server::example::ExampleGame;

namespace {

using Position = std::array<double, 3>;

constexpr double kTickSeconds = 1.0 / 30.0;
// What one tick of full input moves a player.
constexpr double kStepMeters = ExampleGame::kPlayerSpeed / 30.0;
// The server holds positions in floats and sends them in steps of a centimeter: the position a client reads
// is the centre of a step, so a state read back can be off by half of one on each axis (a bit under a step in
// all), and the rule runs in floats on the server.
constexpr double kTolerance = ExampleGame::kPositionStep + 1e-4;

// The movement rule of the example server, for the client: two bytes, the axis along x and the axis along z,
// each as a signed byte cut to -127..127; a short input does nothing. `speed_scale` makes a rule that does
// not agree with the server's.
PredictionRule ExampleRule(double speed_scale = 1.0) {
    return [speed_scale](std::span<double> state, std::span<const uint8_t> input, double dt) {
        if (input.size() < 2) {
            return true;
        }
        const int axis_x = std::max<int>(static_cast<int8_t>(input[0]), -127);
        const int axis_z = std::max<int>(static_cast<int8_t>(input[1]), -127);
        double dx = axis_x / 127.0;
        double dz = axis_z / 127.0;
        const double length = std::sqrt(dx * dx + dz * dz);
        if (length == 0.0) {
            return true;
        }
        if (length > 1.0) {
            dx /= length;
            dz /= length;
        }
        const double speed = ExampleGame::kPlayerSpeed * speed_scale;
        state[0] = std::clamp(state[0] + dx * speed * dt, 0.0, static_cast<double>(ExampleGame::kWorldSide));
        state[2] = std::clamp(state[2] + dz * speed * dt, 0.0, static_cast<double>(ExampleGame::kWorldSide));
        return true;
    };
}

NetworkConditions Steady() {
    return NetworkConditions{};
}

NetworkConditions Delayed() {
    NetworkConditions conditions;
    conditions.min_delay_ticks = 2;
    conditions.max_delay_ticks = 2;
    return conditions;
}

NetworkConditions Jittery(uint32_t seed = 1) {
    NetworkConditions conditions;
    conditions.min_delay_ticks = 0;
    conditions.max_delay_ticks = 2;
    conditions.seed = seed;
    return conditions;
}

NetworkConditions Lossy(uint32_t seed = 1) {
    NetworkConditions conditions = Jittery(seed);
    conditions.loss = 0.3;
    conditions.duplicate = 0.1;
    return conditions;
}

// A host running the example game with players, on a simulated network, and the clients that play.
struct World {
    explicit World(const NetworkConditions& conditions = NetworkConditions{})
        : network(conditions), log(LogLevel::Error, [](const std::string&) {}), game(0, false, true) {
        config.tick_rate_hz = 30;
        config.max_players = 8;
        schema = game.DescribeWorld().schema;
        host = std::make_unique<ServerHost>(config, game, network.Server(), log);
    }

    ClientReplica& AddClient() {
        clients.push_back(std::make_unique<ClientReplica>(*network.AddClient(), *schema));
        return *clients.back();
    }

    // One tick the way a game loop runs it: the network moves, the clients read and let their corrections
    // slide by the time that passed, the host steps.
    void Step(int count = 1) {
        for (int i = 0; i < count; ++i) {
            network.Advance(++now);
            for (const auto& client : clients) {
                client->Poll();
                client->AdvancePrediction(kTickSeconds);
            }
            host->Step();
        }
    }

    EntityId EntityOf(const ClientReplica& client, uint32_t owner) const {
        const TypeSchema* type = schema->Find(ExampleGame::kPlayerType);
        EntityId found = EntityId::Null();
        client.ForEachEntity([&](EntityId id, const uint32_t* words) {
            if (id.type() == ExampleGame::kPlayerType &&
                IntegerValue(type->properties[1], words + type->properties[1].word_offset) == owner) {
                found = id;
            }
        });
        return found;
    }

    // What the server last told the client about the entity.
    Position PositionOf(const ClientReplica& client, EntityId id) const {
        const TypeSchema* type = schema->Find(ExampleGame::kPlayerType);
        const uint32_t* words = client.WordsOf(id);
        Position position{std::nan(""), std::nan(""), std::nan("")};
        if (words != nullptr) {
            DequantizePosition(type->properties[0], words + type->properties[0].word_offset, position.data());
        }
        return position;
    }

    EntityId Join(ClientReplica& client) {
        for (int i = 0; i < 60; ++i) {
            Step();
            if (client.State() == ClientState::Connected &&
                !EntityOf(client, client.ConnectionId()).IsNull()) {
                break;
            }
        }
        Step(3);
        return EntityOf(client, client.ConnectionId());
    }

    SimulatedNetwork network;
    Logger log;
    ExampleGame game;
    tessera::server::ServerConfig config; // spelled out: `net` has a `ServerConfig` too
    const ReplicationSchema* schema = nullptr;
    std::unique_ptr<ServerHost> host;
    std::vector<std::unique_ptr<ClientReplica>> clients;
    uint64_t now = 0;
};

struct Summary {
    size_t count = 0;
    double max = 0.0;
    double mean = 0.0;
    double p95 = 0.0;
    size_t within_tolerance = 0;
};

Summary Summarize(std::vector<double> errors) {
    Summary summary;
    summary.count = errors.size();
    if (errors.empty()) {
        return summary;
    }
    std::sort(errors.begin(), errors.end());
    double total = 0.0;
    for (const double error : errors) {
        total += error;
        summary.within_tolerance += error <= kTolerance ? 1 : 0;
    }
    summary.max = errors.back();
    summary.mean = total / static_cast<double>(errors.size());
    const size_t rank = static_cast<size_t>(std::ceil(0.95 * static_cast<double>(errors.size())));
    summary.p95 = errors[rank == 0 ? 0 : rank - 1];
    return summary;
}

// A player that predicts its own sphere with `rule`, and what happened to the error of each correction.
struct Player {
    World& world;
    ClientReplica& client;
    EntityId entity;
    std::vector<double> errors;      // the error of each correction since `Settle`
    std::vector<uint64_t> snapped;   // 1 for each correction that jumped
    uint64_t seen_corrections = 0;
    uint64_t seen_snaps = 0;
    bool ever_above_snap_without_jump = false;

    explicit Player(World& w, PredictionRule rule = ExampleRule(), double smoothing = 0.1)
        : world(w), client(w.AddClient()), entity(w.Join(client)) {
        PredictionConfig config;
        config.entity = entity;
        config.properties = {0};
        config.rule = std::move(rule);
        config.smoothing_seconds = smoothing;
        TESSERA_CHECK(client.SetPrediction(config, nullptr));
        // The prediction starts with the first state that comes after it is set: the server sends one as soon
        // as it has an input to confirm.
        for (int i = 0; i < 40 && client.Prediction() != PredictionStatus::Active; ++i) {
            Tick(0, 0);
        }
        TESSERA_CHECK(client.Prediction() == PredictionStatus::Active);
        seen_corrections = client.PredictionInfo().reconciliations;
        seen_snaps = client.PredictionInfo().snaps;
        errors.clear();
    }

    // One tick of the game: the input of the player, then the world.
    void Tick(int8_t axis_x, int8_t axis_z) {
        const uint8_t bytes[2] = {static_cast<uint8_t>(axis_x), static_cast<uint8_t>(axis_z)};
        client.SendInput(bytes);
        world.Step();
        const PredictionStats& stats = client.PredictionInfo();
        if (stats.reconciliations != seen_corrections) {
            seen_corrections = stats.reconciliations;
            errors.push_back(stats.last_error);
            const bool jumped = stats.snaps != seen_snaps;
            seen_snaps = stats.snaps;
            // A jump happens exactly when the error is above the snap distance.
            if ((stats.last_error > 2.0) != jumped) {
                ever_above_snap_without_jump = true;
            }
        }
    }

    void Drive(int8_t axis_x, int8_t axis_z, int ticks) {
        for (int i = 0; i < ticks; ++i) {
            Tick(axis_x, axis_z);
        }
    }

    Position Authoritative() const {
        return world.PositionOf(client, entity);
    }

    // The shown position, once the last correction has finished sliding. NaN if the prediction is not active.
    Position Predicted() {
        client.AdvancePrediction(10.0);
        Position shown{std::nan(""), std::nan(""), std::nan("")};
        std::array<double, 3> out{};
        if (client.Predicted(out)) {
            shown = out;
        }
        return shown;
    }
};

double Distance(const Position& a, const Position& b) {
    return std::hypot(a[0] - b[0], a[2] - b[2]);
}

// Ends a run: a second of input with both axes at zero, then a second more, as a game would send them.
void StopAndSettle(Player& player) {
    player.Drive(0, 0, 30);
    player.Drive(0, 0, 30);
}

} // namespace

TESSERA_TEST(with_a_steady_rhythm_and_no_jitter_the_prediction_matches_the_server_within_a_step) {
    World world(Steady());
    Player player(world);
    player.Drive(127, 0, 90);
    const Summary summary = Summarize(player.errors);
    TESSERA_CHECK(summary.count >= 60);
    TESSERA_CHECK(summary.max <= kTolerance);
    // And it really moved: the server's own state has gone the distance.
    const Position shown = player.Predicted();
    TESSERA_CHECK(shown[0] > ExampleGame::PlayerSpawn(player.client.ConnectionId()).first + 8.0);
    TESSERA_CHECK(Distance(shown, player.Authoritative()) < 6 * kStepMeters);
    TESSERA_CHECK(player.client.PredictionInfo().snaps == 0 && !player.ever_above_snap_without_jump);
}

TESSERA_TEST(with_a_constant_delay_of_2_ticks_the_prediction_matches_the_server_within_a_step) {
    World world(Delayed());
    Player player(world);
    player.Drive(127, 0, 90);
    const Summary summary = Summarize(player.errors);
    TESSERA_CHECK(summary.count >= 60);
    TESSERA_CHECK(summary.max <= kTolerance);
    TESSERA_CHECK(player.client.PredictionInfo().snaps == 0);
}

TESSERA_TEST(after_the_input_stops_the_error_is_within_a_step_in_every_scenario) {
    const NetworkConditions scenarios[] = {Steady(), Delayed(), Jittery(), Lossy()};
    for (const NetworkConditions& conditions : scenarios) {
        World world(conditions);
        Player player(world);
        player.Drive(127, 0, 60);
        StopAndSettle(player);
        TESSERA_CHECK(player.client.Prediction() == PredictionStatus::Active);
        TESSERA_CHECK(player.client.PredictionInfo().last_error <= kTolerance);
        const Position shown = player.Predicted();
        TESSERA_CHECK(Distance(shown, player.Authoritative()) <= kTolerance);
        TESSERA_CHECK(player.client.PredictionInfo().history_overflows == 0);
    }
}

TESSERA_TEST(under_jitter_the_error_stays_within_the_measured_bound) {
    for (const uint32_t seed : {1u, 5u, 99u}) {
        World world(Jittery(seed));
        Player player(world);
        player.Drive(127, 0, 300);
        const Summary summary = Summarize(player.errors);
        TESSERA_CHECK(summary.count > 100);
        // Measured at 1.02 and 0.98 steps on these seeds, with a quarter on top.
        TESSERA_CHECK(summary.max <= 1.28 * kStepMeters);
        TESSERA_CHECK(summary.p95 <= 1.23 * kStepMeters);
        TESSERA_CHECK(!player.ever_above_snap_without_jump);
    }
}

TESSERA_TEST(under_30_percent_loss_the_error_stays_below_the_snap_distance_and_within_the_measured_bound) {
    for (const uint32_t seed : {1u, 5u, 99u}) {
        World world(Lossy(seed));
        Player player(world);
        player.Drive(127, 0, 300);
        const Summary summary = Summarize(player.errors);
        TESSERA_CHECK(summary.count > 100);
        TESSERA_CHECK(summary.max < 2.0); // below the distance at which a correction jumps
        // Measured at 4.01 and 2.35 steps on these seeds; the quarter on top is cut to the limits chosen
        // for a very bad network: 5 steps at worst and 3 steps at p95.
        TESSERA_CHECK(summary.max <= 5.0 * kStepMeters);
        TESSERA_CHECK(summary.p95 <= 3.0 * kStepMeters);
        TESSERA_CHECK(!player.ever_above_snap_without_jump);
    }
}

TESSERA_TEST(a_different_rule_is_corrected_by_slide) {
    World world(Steady());
    Player player(world, ExampleRule(1.5));
    player.Drive(127, 0, 60);
    const Summary summary = Summarize(player.errors);
    TESSERA_CHECK(summary.count >= 40 && summary.max > kTolerance); // the rule does not agree: there is error
    TESSERA_CHECK(player.client.PredictionInfo().snaps == 0);
    StopAndSettle(player);
    TESSERA_CHECK(Distance(player.Predicted(), player.Authoritative()) <= kTolerance);
}

TESSERA_TEST(a_very_different_rule_is_corrected_by_a_jump) {
    World world(Steady());
    Player player(world, ExampleRule(10.0));
    player.Drive(127, 0, 60);
    TESSERA_CHECK(player.client.PredictionInfo().snaps > 0);
    // A correction jumps exactly when its error is above the snap distance.
    TESSERA_CHECK(!player.ever_above_snap_without_jump);
    StopAndSettle(player);
    TESSERA_CHECK(Distance(player.Predicted(), player.Authoritative()) <= kTolerance);
}

TESSERA_TEST(a_client_that_stops_sending_is_pulled_back_when_the_server_keeps_the_last_axes) {
    World world(Steady());
    Player player(world);
    player.Drive(127, 0, 30);
    player.errors.clear();
    // The game's contract is one input for each tick, zeros included. A client that stops sending leaves the
    // server holding the last axes (for up to half a second), so the server's sphere keeps going while the
    // predicted one has nothing to apply.
    for (int i = 0; i < 15; ++i) {
        world.Step();
        player.errors.push_back(player.client.PredictionInfo().last_error);
    }
    const Summary during = Summarize(player.errors);
    TESSERA_CHECK(during.max > 4 * kTolerance); // the error is more than a step of the quantization
    const Position far = player.Predicted();
    TESSERA_CHECK(Distance(far, player.Authoritative()) <= 2.0 * kStepMeters + kTolerance);
    // Once the server stops too, the two agree again.
    world.Step(60);
    TESSERA_CHECK(Distance(player.Predicted(), player.Authoritative()) <= kTolerance);
}

TESSERA_TEST(an_idle_player_sending_zero_inputs_keeps_the_prediction_active) {
    World world(Steady());
    Player player(world);
    player.Drive(0, 0, 200);
    TESSERA_CHECK(player.client.Prediction() == PredictionStatus::Active);
    TESSERA_CHECK(player.client.PredictionInfo().history_overflows == 0);
    TESSERA_CHECK(player.client.PredictionPending() < 8);
    TESSERA_CHECK(world.host->Replication().Stats().input_confirmations_sent > 100);
    TESSERA_CHECK(Distance(player.Predicted(), player.Authoritative()) <= kTolerance);
}

TESSERA_TEST(a_long_outage_fills_the_history_and_prediction_resumes) {
    World world(Steady());
    Player player(world);
    player.Drive(127, 0, 30);
    // The network goes away for three seconds while the player keeps pressing.
    NetworkConditions outage;
    outage.loss = 1.0;
    world.network.SetConditions(outage);
    player.Drive(127, 0, 90);
    TESSERA_CHECK(player.client.PredictionInfo().history_overflows >= 1);
    TESSERA_CHECK(player.client.Prediction() == PredictionStatus::Paused);
    TESSERA_CHECK(player.client.PredictionPending() <= Predictor::kHistoryCapacity);
    // It comes back.
    world.network.SetConditions(Steady());
    player.Drive(127, 0, 30);
    TESSERA_CHECK(player.client.Prediction() == PredictionStatus::Active);
    StopAndSettle(player);
    TESSERA_CHECK(player.client.Prediction() == PredictionStatus::Active);
    TESSERA_CHECK(Distance(player.Predicted(), player.Authoritative()) <= kTolerance);
}

TESSERA_TEST(two_predicting_players_do_not_disturb_each_other) {
    World world(Delayed());
    Player a(world);
    Player b(world);
    for (int i = 0; i < 90; ++i) {
        const uint8_t a_input[2] = {127, 0};
        const uint8_t b_input[2] = {0, 127};
        a.client.SendInput(a_input);
        b.client.SendInput(b_input);
        world.Step();
    }
    world.Step(3);
    for (Player* player : {&a, &b}) {
        TESSERA_CHECK(player->client.Prediction() == PredictionStatus::Active);
        TESSERA_CHECK(player->client.PredictionInfo().snaps == 0);
    }
    const Position a_start = ExampleGame::PlayerSpawn(a.client.ConnectionId()).first > 0
                                 ? Position{ExampleGame::PlayerSpawn(a.client.ConnectionId()).first, 0.0,
                                            ExampleGame::PlayerSpawn(a.client.ConnectionId()).second}
                                 : Position{};
    const Position b_start = {ExampleGame::PlayerSpawn(b.client.ConnectionId()).first, 0.0,
                              ExampleGame::PlayerSpawn(b.client.ConnectionId()).second};
    const Position a_shown = a.Predicted();
    const Position b_shown = b.Predicted();
    TESSERA_CHECK(a_shown[0] > a_start[0] + 6.0 && std::fabs(a_shown[2] - a_start[2]) < 0.1);
    TESSERA_CHECK(b_shown[2] > b_start[2] + 6.0 && std::fabs(b_shown[0] - b_start[0]) < 0.1);
    // Each one is close to what the server holds for it (a few steps ahead of the last state that arrived).
    TESSERA_CHECK(Distance(a_shown, a.Authoritative()) < 8 * kStepMeters);
    TESSERA_CHECK(Distance(b_shown, b.Authoritative()) < 8 * kStepMeters);
    // And each settles on its own entity when they stop.
    for (int i = 0; i < 60; ++i) {
        const uint8_t zeros[2] = {0, 0};
        a.client.SendInput(zeros);
        b.client.SendInput(zeros);
        world.Step();
    }
    TESSERA_CHECK(Distance(a.Predicted(), a.Authoritative()) <= kTolerance);
    TESSERA_CHECK(Distance(b.Predicted(), b.Authoritative()) <= kTolerance);
}
