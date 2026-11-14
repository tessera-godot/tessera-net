// SPDX-License-Identifier: Apache-2.0
// Interpolation against the example server on a simulated network: a client watches another player move and
// shows it a little in the past, smoothed, whatever the network does. Some of the tests measure (gaps under a
// bandwidth budget, the longest jump between two blended states) and keep only what cannot change.
#include "example_game.hpp"
#include "tessera/net/client_replica.hpp"
#include "tessera/net/entity_table.hpp"
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
#include <utility>
#include <vector>

using namespace tessera::net;
using namespace tessera::server;
using tessera::server::example::ExampleGame;

namespace {

constexpr double kTickSeconds = 1.0 / 30.0;
// What one tick of full input moves a player.
constexpr double kStepMeters = ExampleGame::kPlayerSpeed / 30.0;
// A state read back is the centre of a step of a centimeter: it can be off by half of one, and two of them
// by a whole step (plus what a float loses on the way).
constexpr double kTolerance = ExampleGame::kPositionStep + 1e-4;

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

InterpolationConfig Delay(uint32_t ticks) {
    InterpolationConfig config;
    config.delay_ticks = ticks;
    return config;
}

// What the observing client showed in one frame, and what it held.
struct Frame {
    double display = 0.0;    // the tick shown
    uint64_t latest = 0;     // the newest tick applied
    double x = 0.0;          // the position shown
    double newest_x = 0.0;   // the position of the newest state
    uint64_t state_tick = 0; // the tick of the newest state
};

// A state as the observer received it, in the order the newest changed.
struct State {
    uint64_t tick;
    double x;
};

// A player that drives (A) and a client that watches it (B) with interpolation on, against the example
// server.
struct Scene {
    explicit Scene(const NetworkConditions& conditions, const InterpolationConfig& config = Delay(2))
        : network(conditions), log(LogLevel::Error, [](const std::string&) {}), game(0, false, true) {
        server_config.tick_rate_hz = 30;
        server_config.max_players = 8;
        schema = game.DescribeWorld().schema;
        host = std::make_unique<ServerHost>(server_config, game, network.Server(), log);
        a = std::make_unique<ClientReplica>(*network.AddClient(), *schema);
        b = std::make_unique<ClientReplica>(*network.AddClient(), *schema);
        TESSERA_CHECK(b->SetInterpolation(config, nullptr));
        for (int i = 0; i < 80; ++i) {
            Tick(0, 0);
            entity = EntityOf(*b, a->ConnectionId());
            if (a->State() == ClientState::Connected && !entity.IsNull() &&
                b->StateTickOf(entity) > 0) {
                break;
            }
        }
        for (int i = 0; i < 5; ++i) {
            Tick(0, 0);
        }
        TESSERA_CHECK(!entity.IsNull());
        frames.clear();
        states.clear();
        last_state_tick = 0;
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

    double NewestX() const {
        const TypeSchema* type = schema->Find(ExampleGame::kPlayerType);
        const uint32_t* words = b->WordsOf(entity);
        double position[3] = {std::nan(""), 0.0, 0.0};
        if (words != nullptr) {
            DequantizePosition(type->properties[0], words + type->properties[0].word_offset, position);
        }
        return position[0];
    }

    // One tick the way a game loop runs it: the player sends its input, the network moves, the clients read,
    // and the watcher shows `frames_per_tick` frames before the server steps.
    void Tick(int8_t axis_x, int8_t axis_z, int frames_per_tick = 1) {
        const uint8_t bytes[2] = {static_cast<uint8_t>(axis_x), static_cast<uint8_t>(axis_z)};
        a->SendInput(bytes);
        network.Advance(++now);
        a->Poll();
        b->Poll();
        for (int frame = 0; frame < frames_per_tick; ++frame) {
            b->AdvanceInterpolation(kTickSeconds / frames_per_tick);
            Sample();
        }
        host->Step();
    }

    void Drive(int8_t axis_x, int8_t axis_z, int ticks, int frames_per_tick = 1) {
        for (int i = 0; i < ticks; ++i) {
            Tick(axis_x, axis_z, frames_per_tick);
        }
    }

    void Sample() {
        if (entity.IsNull()) {
            return;
        }
        double out[3] = {};
        if (!b->DisplayedValue(entity, 0, out)) {
            return;
        }
        Frame frame;
        frame.display = b->DisplayedTick();
        frame.latest = b->LatestTick();
        frame.x = out[0];
        frame.newest_x = NewestX();
        frame.state_tick = b->StateTickOf(entity);
        frames.push_back(frame);
        if (frame.state_tick != last_state_tick && frame.state_tick != 0) {
            last_state_tick = frame.state_tick;
            states.push_back(State{frame.state_tick, frame.newest_x});
        }
    }

    SimulatedNetwork network;
    Logger log;
    ExampleGame game;
    tessera::server::ServerConfig server_config; // spelled out: `net` has a `ServerConfig` too
    const ReplicationSchema* schema = nullptr;
    std::unique_ptr<ServerHost> host;
    std::unique_ptr<ClientReplica> a;
    std::unique_ptr<ClientReplica> b;
    EntityId entity = EntityId::Null(); // the entity of A, as B knows it
    std::vector<Frame> frames;
    std::vector<State> states;
    uint64_t last_state_tick = 0;
    uint64_t now = 0;
};

// The path a player at full speed follows: x grows by `kStepMeters` a tick. The offset is fitted to the
// states that were received while it moved.
struct Path {
    uint64_t first_tick = 0;
    double offset = 0.0;
    size_t first_state = 0;

    static Path Fit(const std::vector<State>& states) {
        Path path;
        for (size_t i = 1; i < states.size(); ++i) {
            if (states[i].x - states[i - 1].x > 0.2) {
                path.first_state = i; // the first state that is on the path: the one before stood still
                path.first_tick = states[i].tick;
                break;
            }
        }
        double total = 0.0;
        size_t count = 0;
        for (size_t i = path.first_state; i < states.size(); ++i) {
            total += states[i].x - kStepMeters * static_cast<double>(states[i].tick - path.first_tick);
            ++count;
        }
        path.offset = count > 0 ? total / static_cast<double>(count) : 0.0;
        return path;
    }

    double At(double tick) const {
        return offset + kStepMeters * (tick - static_cast<double>(first_tick));
    }
};

} // namespace

TESSERA_TEST(with_constant_speed_and_no_loss_the_shown_position_is_on_the_true_path) {
    for (const uint32_t delay : {0u, 2u}) {
        for (const int frames_per_tick : {1, 2}) {
            Scene scene(Steady(), Delay(delay));
            scene.Drive(127, 0, 90, frames_per_tick);
            const Path path = Path::Fit(scene.states);
            TESSERA_CHECK(path.first_tick > 0 && scene.states.size() > 60);
            size_t checked = 0;
            const uint64_t last = scene.states.back().tick;
            for (const Frame& frame : scene.frames) {
                if (frame.display < static_cast<double>(path.first_tick) ||
                    frame.display > static_cast<double>(last)) {
                    continue;
                }
                ++checked;
                TESSERA_CHECK(std::fabs(frame.x - path.At(frame.display)) <= kTolerance);
                // The time shown follows the newest tick at the delay, within one tick of play.
                const double lag = static_cast<double>(frame.latest) - frame.display;
                // (With no delay it is the newest tick the clock waits for: up to one tick behind it.)
                TESSERA_CHECK(lag >= (delay > 0 ? delay - 1.0 : 0.0) - 1e-9 &&
                              lag <= (delay > 0 ? delay : 1.0) + 1e-9);
            }
            TESSERA_CHECK(checked >= 60);
            TESSERA_CHECK(scene.b->InterpolationInfo().snaps == 0);
            TESSERA_CHECK(scene.b->InterpolationInfo().gap_holds == 0);
        }
    }
}

TESSERA_TEST(with_a_constant_delay_the_shown_value_is_monotonic_and_does_not_overshoot) {
    Scene scene(Delayed(), Delay(2));
    scene.Drive(127, 0, 90, 2);
    TESSERA_CHECK(scene.frames.size() > 150);
    double previous = scene.frames.front().x;
    for (const Frame& frame : scene.frames) {
        TESSERA_CHECK(frame.x >= previous - 1e-9);
        TESSERA_CHECK(frame.x <= frame.newest_x + 1e-9); // never beyond the newest state
        TESSERA_CHECK(static_cast<double>(frame.latest) - frame.display <= 3.0);
        TESSERA_CHECK(frame.display <= static_cast<double>(frame.latest));
        previous = frame.x;
    }
    TESSERA_CHECK(scene.frames.back().x > scene.frames.front().x + 10.0);
}

namespace {

// What every run on a bad network must keep: the shown position never goes back by more than a step (a late
// state can pull it back to its own segment), stays within what was received, and never passes the newest
// tick. Returns how many times the newest state skipped a tick.
size_t CheckBounded(const Scene& scene) {
    // The time shown keeps up with the newest tick (it is not left behind, nor ahead of it).
    const Frame& first = scene.frames.front();
    const Frame& last_frame = scene.frames.back();
    TESSERA_CHECK(last_frame.display - first.display >=
                  0.9 * static_cast<double>(last_frame.latest - first.latest));
    double low = 1e9;
    double high = -1e9;
    double previous = scene.frames.empty() ? 0.0 : scene.frames.front().x;
    size_t skipped = 0;
    uint64_t last_tick = 0;
    for (const Frame& frame : scene.frames) {
        low = std::min(low, frame.newest_x);
        high = std::max(high, frame.newest_x);
        TESSERA_CHECK(frame.x >= low - 1e-9 && frame.x <= high + 1e-9);
        TESSERA_CHECK(frame.x >= previous - ExampleGame::kPositionStep - 1e-9);
        TESSERA_CHECK(frame.display <= static_cast<double>(frame.latest));
        previous = frame.x;
        if (last_tick != 0 && frame.state_tick > last_tick + 1) {
            ++skipped;
        }
        last_tick = std::max(last_tick, frame.state_tick);
    }
    return skipped;
}

} // namespace

TESSERA_TEST(under_jitter_the_shown_position_stays_between_two_received_states_and_never_goes_back) {
    for (const uint32_t seed : {1u, 5u, 99u}) {
        Scene scene(Jittery(seed), Delay(2));
        scene.Drive(127, 0, 150, 2);
        TESSERA_CHECK(scene.frames.size() > 250);
        CheckBounded(scene);
    }
}

TESSERA_TEST(under_30_percent_loss_the_shown_position_stays_between_two_received_states_and_never_goes_back) {
    for (const uint32_t seed : {1u, 5u, 99u}) {
        Scene scene(Lossy(seed), Delay(2));
        scene.Drive(127, 0, 200, 2);
        TESSERA_CHECK(scene.frames.size() > 300);
        const size_t skipped = CheckBounded(scene);
        TESSERA_CHECK(skipped > 10); // a lost state only makes a segment longer
        // Where the shown time is inside a segment of the path, the position is on the line between the
        // states around it.
        const Path path = Path::Fit(scene.states);
        const uint64_t last = scene.states.back().tick;
        size_t on_the_line = 0;
        size_t inside = 0;
        for (const Frame& frame : scene.frames) {
            if (frame.display < static_cast<double>(path.first_tick) ||
                frame.display > static_cast<double>(last)) {
                continue;
            }
            ++inside;
            on_the_line += std::fabs(frame.x - path.At(frame.display)) <= 2.0 * kTolerance ? 1 : 0;
        }
        TESSERA_CHECK(inside > 200);
        // A state that is held instead of blended (a long gap) is the exception, not the rule.
        TESSERA_CHECK(on_the_line * 100 >= inside * 97);
    }
}

TESSERA_TEST(a_state_later_than_the_delay_makes_the_entity_wait_for_it) {
    NetworkConditions conditions;
    conditions.min_delay_ticks = 0;
    conditions.max_delay_ticks = 5;
    conditions.seed = 3;
    Scene scene(conditions, Delay(2));
    scene.Drive(127, 0, 200, 1);
    CheckBounded(scene);
    const InterpolationStats& stats = scene.b->InterpolationInfo();
    TESSERA_CHECK(stats.stalls > 0);
    double biggest_jump = 0.0;
    for (size_t i = 1; i < scene.frames.size(); ++i) {
        biggest_jump = std::max(biggest_jump, scene.frames[i].x - scene.frames[i - 1].x);
    }
    // The entity waits for a late state and then goes on; the most it moves in one frame is a step of the
    // path at the fastest the clock runs (a tenth more).
    TESSERA_CHECK(biggest_jump <= kStepMeters * 1.1 + kTolerance);
}

TESSERA_TEST(a_player_that_stood_still_does_not_creep_before_it_moves) {
    Scene scene(Steady(), Delay(2));
    scene.Drive(127, 0, 20); // it walks a little
    scene.Drive(0, 0, 90);   // then stands still for three seconds: the server says nothing about it
    scene.Drive(127, 0, 30); // and walks again
    // The gap between the last state of the first walk and the first of the second.
    size_t second = 0;
    for (size_t i = 1; i < scene.states.size(); ++i) {
        if (scene.states[i].tick > scene.states[i - 1].tick + 60 &&
            scene.states[i].x - scene.states[i - 1].x > 0.2) {
            second = i;
            break;
        }
    }
    TESSERA_CHECK(second > 0);
    if (second == 0) {
        return;
    }
    const State stopped = scene.states[second - 1];
    const State moved = scene.states[second];
    TESSERA_CHECK(moved.tick - stopped.tick >= 60);
    size_t held = 0;
    for (const Frame& frame : scene.frames) {
        if (frame.display >= static_cast<double>(stopped.tick) &&
            frame.display < static_cast<double>(moved.tick)) {
            ++held;
            TESSERA_CHECK(std::fabs(frame.x - stopped.x) < 1e-12); // exactly where it stood
        }
    }
    TESSERA_CHECK(held >= 1);
    TESSERA_CHECK(scene.b->InterpolationInfo().gap_holds >= 1);
    TESSERA_CHECK(scene.b->InterpolationInfo().resyncs >= 1); // the clock did not replay the silence
    // And once the new state is reached the entity is on its way again.
    TESSERA_CHECK(scene.frames.back().x > stopped.x + 2.0);
}

TESSERA_TEST(measure_snap_against_the_fastest_player) {
    for (const uint32_t seed : {1u, 5u, 99u}) {
        Scene scene(Lossy(seed), Delay(2));
        scene.Drive(127, 0, 300, 1);
        const InterpolationStats& stats = scene.b->InterpolationInfo();
        uint64_t longest_linked_gap = 0;
        double longest_linked_move = 0.0;
        for (size_t i = 1; i < scene.states.size(); ++i) {
            const uint64_t gap = scene.states[i].tick - scene.states[i - 1].tick;
            if (gap > 8) {
                continue;
            }
            longest_linked_gap = std::max(longest_linked_gap, gap);
            longest_linked_move =
                std::max(longest_linked_move, std::fabs(scene.states[i].x - scene.states[i - 1].x));
        }
        TESSERA_CHECK(stats.snaps == 0);
        TESSERA_CHECK(longest_linked_gap >= 1 && longest_linked_gap <= 8);
        TESSERA_CHECK(stats.gap_holds * 200 <= stats.states); // at most 0.5 % of the states
        // The default snap distance must stay above the most a player moves inside the longest gap that is
        // still blended: 8 ticks at full speed.
        TESSERA_CHECK(longest_linked_move <= kStepMeters * 8.0 + 0.02);
        TESSERA_CHECK(longest_linked_move < InterpolationConfig{}.snap_distance);
    }
}

TESSERA_TEST(the_entity_stops_when_the_driver_stops_and_converges_to_the_last_value) {
    Scene scene(Steady(), Delay(2));
    scene.Drive(127, 0, 60);
    scene.Drive(0, 0, 45, 2); // the server keeps the last axes for half a second, then the entity stops
    TESSERA_CHECK(!scene.frames.empty());
    const Frame& last = scene.frames.back();
    TESSERA_CHECK(std::fabs(last.x - last.newest_x) < 1e-9);
    TESSERA_CHECK(last.display == static_cast<double>(last.latest)); // nothing newer is coming
    double out[3] = {};
    TESSERA_CHECK(scene.b->DisplayedValue(scene.entity, 0, out));
    TESSERA_CHECK(std::fabs(out[0] - scene.NewestX()) < 1e-9);
}

// ----- a game of entities that walk by themselves, for the tests that need a bandwidth budget -----

namespace {

struct WalkColumns {
    float x[kPageSlots];
    float y[kPageSlots];
    float z[kPageSlots];
};

// `count` entities that walk along x at `speed` meters a second, turning back at the ends of a 200 m line.
// The example game sets no budget, so these tests bring their own.
class WalkerGame final : public IGameLogic {
public:
    WalkerGame(uint32_t count, float speed, uint32_t rate_kbps)
        : count_(count), speed_(speed), rate_kbps_(rate_kbps) {
        schema_.AddType(
            1, {PropertyDescriptor::Position(TESSERA_COLUMN(WalkColumns, x), TESSERA_COLUMN(WalkColumns, y),
                                             TESSERA_COLUMN(WalkColumns, z), 0.01f, 2048.0f)});
    }

    WorldSetup DescribeWorld() override {
        WorldSetup setup;
        setup.schema = &schema_;
        setup.relevancy.types[1] = TypeRelevancy{RelevancyMode::AlwaysRelevant, 0.0f, 0.0f};
        setup.replication.max_visible_per_connection = 1024;
        setup.replication.rate_kbps = rate_kbps_;
        setup.replication.peak_kbps = rate_kbps_;
        setup.replication.max_buffer_bytes = 65535;
        return setup;
    }

    void OnStart(ServerContext& context) override {
        context_ = &context;
        table_ = std::make_unique<EntityTable<WalkColumns>>(TableConfig{context.shard_id, 1});
        for (uint32_t i = 0; i < count_; ++i) {
            const EntityId id = table_->Create();
            const auto found = table_->Find(id);
            WalkColumns& columns = table_->Columns(found->page);
            const float x = 10.0f + static_cast<float>(i % 100) * 1.7f;
            columns.x[found->slot] = x;
            columns.y[found->slot] = 0.0f;
            columns.z[found->slot] = static_cast<float>(i / 100) * 3.0f;
            table_->MarkDirty(id);
            context.grid.Insert(id, x, 0.0f, columns.z[found->slot]);
            context.replication.Relevancy().OnEntityCreated(id);
            ids_.push_back(id);
            where_.push_back(SlotLocation{found->page, found->slot});
            direction_.push_back(1.0f);
        }
    }

    void OnTick(double dt_seconds) override {
        moves_.clear();
        for (size_t i = 0; i < ids_.size(); ++i) {
            WalkColumns& columns = table_->Columns(where_[i].page);
            float x = columns.x[where_[i].slot] + direction_[i] * speed_ * static_cast<float>(dt_seconds);
            if (x < 0.0f || x > 200.0f) {
                direction_[i] = -direction_[i];
                x = std::clamp(x, 0.0f, 200.0f);
            }
            columns.x[where_[i].slot] = x;
            table_->MarkDirty(ids_[i]);
            moves_.push_back(PendingMove{ids_[i], x, 0.0f, columns.z[where_[i].slot]});
        }
        context_->grid.ApplyMoves(moves_);
        context_->replication.Refresh(*table_);
    }

    const ReplicationSchema& Schema() const {
        return schema_;
    }

private:
    uint32_t count_;
    float speed_;
    uint32_t rate_kbps_;
    ReplicationSchema schema_;
    ServerContext* context_ = nullptr;
    std::unique_ptr<EntityTable<WalkColumns>> table_;
    std::vector<EntityId> ids_;
    std::vector<SlotLocation> where_;
    std::vector<float> direction_;
    std::vector<PendingMove> moves_;
};

// A host running the walkers, and one client that watches them with interpolation on.
struct WalkWorld {
    WalkWorld(uint32_t count, float speed, uint32_t rate_kbps, const NetworkConditions& conditions = Steady(),
              const InterpolationConfig& config = Delay(2))
        : network(conditions), log(LogLevel::Error, [](const std::string&) {}),
          game(count, speed, rate_kbps) {
        server_config.tick_rate_hz = 30;
        server_config.max_players = 4;
        host = std::make_unique<ServerHost>(server_config, game, network.Server(), log);
        client = std::make_unique<ClientReplica>(*network.AddClient(), game.Schema());
        TESSERA_CHECK(client->SetInterpolation(config, nullptr));
        for (int i = 0; i < 40 && client->VisibleCount() < count; ++i) {
            Tick();
        }
    }

    double X(EntityId id) const {
        double out[3] = {std::nan(""), 0.0, 0.0};
        client->DisplayedValue(id, 0, out);
        return out[0];
    }

    // One tick: the network moves, the client reads and shows `frames` frames, the host steps. Returns the
    // bytes the server wrote for the client in the tick before.
    void Tick(int frames = 1) {
        network.Advance(++now);
        client->Poll();
        for (int frame = 0; frame < frames; ++frame) {
            client->AdvanceInterpolation(kTickSeconds / frames);
        }
        host->Step();
        written = host->Replication().Replication().Output(client->ConnectionId()).size();
    }

    SimulatedNetwork network;
    Logger log;
    WalkerGame game;
    tessera::server::ServerConfig server_config;
    std::unique_ptr<ServerHost> host;
    std::unique_ptr<ClientReplica> client;
    size_t written = 0;
    uint64_t now = 0;
};

struct GapStats {
    size_t count = 0;
    uint64_t p50 = 0;
    uint64_t p95 = 0;
    uint64_t p99 = 0;
    uint64_t max = 0;
};

GapStats Quantiles(std::vector<uint64_t> gaps) {
    GapStats stats;
    stats.count = gaps.size();
    if (gaps.empty()) {
        return stats;
    }
    std::sort(gaps.begin(), gaps.end());
    auto at = [&](double q) {
        const size_t rank = static_cast<size_t>(std::ceil(q * static_cast<double>(gaps.size())));
        return gaps[rank == 0 ? 0 : rank - 1];
    };
    stats.p50 = at(0.50);
    stats.p95 = at(0.95);
    stats.p99 = at(0.99);
    stats.max = gaps.back();
    return stats;
}

// The gaps, in ticks, between the states of each entity as the client received them over `ticks` ticks.
GapStats MeasureGaps(WalkWorld& world, int ticks, double* bytes_per_tick, InterpolationStats* seen) {
    std::vector<EntityId> entities;
    world.client->ForEachEntity([&](EntityId id, const uint32_t*) { entities.push_back(id); });
    std::vector<uint64_t> last(entities.size(), 0);
    for (size_t i = 0; i < entities.size(); ++i) {
        last[i] = world.client->StateTickOf(entities[i]);
    }
    std::vector<uint64_t> gaps;
    double bytes = 0.0;
    for (int tick = 0; tick < ticks; ++tick) {
        world.Tick();
        bytes += static_cast<double>(world.written);
        for (size_t i = 0; i < entities.size(); ++i) {
            const uint64_t now_tick = world.client->StateTickOf(entities[i]);
            if (now_tick > last[i]) {
                gaps.push_back(now_tick - last[i]);
                last[i] = now_tick;
            }
        }
    }
    if (bytes_per_tick != nullptr) {
        *bytes_per_tick = bytes / ticks;
    }
    if (seen != nullptr) {
        *seen = world.client->InterpolationInfo();
    }
    return Quantiles(gaps);
}

} // namespace

TESSERA_TEST(measure_deferred_gaps_under_a_budget) {
    constexpr uint32_t kEntities = 200;
    constexpr float kSpeed = 5.0f;
    // First without a budget: what the server writes for every entity every tick.
    double unlimited_bytes = 0.0;
    {
        WalkWorld world(kEntities, kSpeed, 0);
        world.Tick(1);
        world.Tick(1);
        InterpolationStats stats;
        const GapStats gaps = MeasureGaps(world, 120, &unlimited_bytes, &stats);
        TESSERA_CHECK(gaps.count > 0);
        TESSERA_CHECK(gaps.max == 1 && gaps.p99 == 1); // without a budget every entity is in every tick
        TESSERA_CHECK(stats.gap_holds == 0 && stats.snaps == 0);
    }
    TESSERA_CHECK(unlimited_bytes > 0.0);
    for (const double share : {0.5, 0.25}) {
        // kilobits a second that carry `share` of those bytes at 30 ticks a second.
        const uint32_t rate = std::max<uint32_t>(
            16, static_cast<uint32_t>(std::ceil(unlimited_bytes * share * 30.0 * 8.0 / 1000.0)));
        WalkWorld world(kEntities, kSpeed, rate);
        for (int i = 0; i < 90; ++i) {
            world.Tick(); // let the queue of what waits settle
        }
        double bytes = 0.0;
        InterpolationStats stats;
        const GapStats gaps = MeasureGaps(world, 240, &bytes, &stats);
        TESSERA_CHECK(gaps.count > 0);
        if (share == 0.5) {
            // Half of what the entities need to go out every tick: they take turns, a few ticks each.
            TESSERA_CHECK(gaps.p50 <= 3);
            TESSERA_CHECK(gaps.p99 <= 8); // below the gap limit, so they slide
        }
    }
}

TESSERA_TEST(a_stopped_network_holds_the_value_and_resumes_without_sliding_the_gap) {
    WalkWorld world(1, 8.0f, 0);
    EntityId entity = EntityId::Null();
    world.client->ForEachEntity([&](EntityId id, const uint32_t*) { entity = id; });
    TESSERA_CHECK(!entity.IsNull());
    for (int i = 0; i < 30; ++i) {
        world.Tick(2);
    }
    // The network loses everything for three seconds: the entity goes on walking on the server. The shown
    // time first catches up with the newest state that arrived, then it waits there.
    NetworkConditions dead;
    dead.loss = 1.0;
    world.network.SetConditions(dead);
    for (int i = 0; i < 5; ++i) {
        world.Tick(2);
    }
    const double held = world.X(entity);
    double moved_during = 0.0;
    for (int i = 0; i < 85; ++i) {
        world.Tick(2);
        moved_during = std::max(moved_during, std::fabs(world.X(entity) - held));
    }
    TESSERA_CHECK(moved_during < 1e-9); // it stays where it was shown last: nothing is invented
    TESSERA_CHECK(world.client->DisplayedTick() <= static_cast<double>(world.client->LatestTick()));
    world.network.SetConditions(Steady());
    const uint64_t resyncs_before = world.client->InterpolationInfo().resyncs;
    std::vector<double> after;
    for (int i = 0; i < 40; ++i) {
        world.Tick(2);
        after.push_back(world.X(entity));
    }
    TESSERA_CHECK(world.client->InterpolationInfo().resyncs > resyncs_before);
    // The first state after the outage is far from the last one before it (3 seconds of walking). It is not
    // slid through: every frame shows the old place or something at or beyond the new state.
    size_t between = 0;
    bool jumped = false;
    for (const double x : after) {
        if (!jumped && std::fabs(x - held) < 1e-9) {
            continue; // still where it stood
        }
        if (!jumped) {
            jumped = true;
            TESSERA_CHECK(x - held > 20.0); // the first thing shown after the old place is far on
        } else if (x < held + 20.0) {
            ++between;
        }
    }
    TESSERA_CHECK(jumped && between == 0);
}
