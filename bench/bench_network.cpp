// SPDX-License-Identifier: Apache-2.0
// Benchmark of the whole network path: a replication server with real relevancy and replication, 128 clients
// and a simulated network that loses 5% of what it carries and delays every message by one or two ticks.
// The server sees a world of 200,000 entities in four spatial types, a sixth of which change every tick, and
// 128 players who walk and drive through it. Everything the server does in a tick is measured: quantizing
// what changed, relevancy, replication, the protocol and the transport. So is everything it sends, by kind.
#include "bench_network.hpp"

#include "bench_memory.hpp"
#include "bench_replication.hpp"
#include "bench_report.hpp"
#include "tessera/net/client_replica.hpp"
#include "tessera/net/entity_table.hpp"
#include "tessera/net/job_system.hpp"
#include "tessera/net/profile.hpp"
#include "tessera/net/protocol.hpp"
#include "tessera/net/replication_server.hpp"
#include "tessera/net/transport/simulated_network.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <span>
#include <vector>

using namespace tessera::net;
using bench::Clock;
using bench::Info;
using bench::Median;
using bench::Ms;
using bench::Report;
using bench::ReportBool;

namespace {

// Regression limits: between 1.6 and 1.9 times what the path measures, so that run-to-run variation does not fail the
// benchmark but a real slowdown does. They are not the goals. The goals were 3.3 ms per tick with one thread,
// 1 ms with eight workers and 100 kbps per player; the path does not meet them yet.
constexpr double kLimitOneThreadMs = 50.0;
constexpr double kLimitEightWorkersMs = 16.0;
// What the budget promises, from the goal of 100 kbps on average and 200 kbps over any 10 seconds, with room
// for the headers and the event batches that the budget does not hold back.
constexpr double kLimitSteadyKbps = 105.0;
constexpr double kLimitPeakKbps = 205.0;
constexpr double kLimitResendFactor = 1.5; // records written per change when only the pacing is on
constexpr int kLimitJoinTicks = 90;        // 3 seconds to receive everything a new player sees
constexpr double kLimitClientPollUs = 60.0;
constexpr double kLimitMbPerPlayer = 3.6;

constexpr uint32_t kTickRate = 30;
constexpr uint32_t kWarmupTicks = 20;
constexpr int kRuns =
    23; // 460 ticks: a little more than 10 seconds for the budget to settle, and one more window
constexpr int kWindowTicks = 10 * kTickRate;
constexpr int kSteadyTicks =
    5 * kTickRate; // the average is taken over the last of them, with the burst used up
constexpr int kTicksPerRun = 20;
constexpr int kJoinLimit = 600;   // ticks a join may take before it is called failed
constexpr int kSettleLimit = 600; // the same, for the clients catching up after the world stops

// Four spatial types; most entities are small things seen from close by, a few are seen from far.
constexpr float kRanges[4] = {100.0f, 250.0f, 400.0f, 600.0f};
constexpr float kMargin = 8.0f;
constexpr uint32_t kMaxVisible = 4096;

// Movement at 30 ticks per second: observers walk at 5 m/s and a tenth of them drive at 25 m/s; the entities
// that change take a step of 0.2 m.
constexpr float kPi = 3.14159265f;
constexpr float kWalkStep = 5.0f / 30.0f;
constexpr float kFastStep = 25.0f / 30.0f;
constexpr float kEntityStep = 0.2f;

struct Scenario {
    uint32_t players;
    uint32_t entities;
    float world_size;       // side of the square world, in metres
    uint32_t change_period; // each entity changes once every this many ticks
};

// The main scenario has the density of the relevancy benchmark (10,000 entities per square km). An entity
// changes about five times a second, as in the typical case of the replication benchmark.
constexpr Scenario kMain{128, 200'000, 4500.0f, 6};
constexpr Scenario kSmall{16, 20'000, 1400.0f, 6};

struct BenchColumns {
    float x[kPageSlots];
    float y[kPageSlots];
    float z[kPageSlots];
    float yaw[kPageSlots];
    int32_t health[kPageSlots];
    uint16_t flags[kPageSlots];
    uint8_t state[kPageSlots];
};
using Table = EntityTable<BenchColumns>;

ReplicationSchema MakeSchema() {
    ReplicationSchema schema;
    for (uint32_t type = 0; type < 4; ++type) {
        schema.AddType(
            type,
            {PropertyDescriptor::Position(TESSERA_COLUMN(BenchColumns, x), TESSERA_COLUMN(BenchColumns, y),
                                          TESSERA_COLUMN(BenchColumns, z), 0.01f, 10000.0f),
             PropertyDescriptor::Angle(TESSERA_COLUMN(BenchColumns, yaw), 10),
             PropertyDescriptor::Integer(TESSERA_COLUMN(BenchColumns, health), true, -1000, 1000),
             PropertyDescriptor::Bits(TESSERA_COLUMN(BenchColumns, flags), 16),
             PropertyDescriptor::Enum(TESSERA_COLUMN(BenchColumns, state), 8)});
    }
    return schema;
}

RelevancyConfig MakeRelevancy() {
    RelevancyConfig config;
    for (uint32_t t = 0; t < 4; ++t) {
        config.types[t] = TypeRelevancy{RelevancyMode::Spatial, kRanges[t], kMargin};
    }
    return config;
}

NetworkConditions Conditions(double loss) {
    NetworkConditions conditions;
    conditions.loss = loss;
    conditions.min_delay_ticks = 1;
    conditions.max_delay_ticks = 2;
    conditions.seed = 2024;
    return conditions;
}

/// Passes everything to the real transport and counts what the server sends and receives, by message kind.
/// It can also hash every byte sent, to compare two runs.
class CountingTransport final : public ITransport {
public:
    CountingTransport(ITransport& inner, bool hash_bytes) : inner_(inner), hash_bytes_(hash_bytes) {}

    void Poll(std::vector<TransportEvent>& out) override {
        const size_t before = out.size();
        inner_.Poll(out);
        for (size_t i = before; i < out.size(); ++i) {
            if (out[i].kind == TransportEvent::Kind::Message) {
                received_[Slot(out[i].data)] += out[i].data.size();
            }
        }
    }
    bool Send(TransportConnection connection, Channel channel, std::span<const uint8_t> data) override {
        const bool sent = inner_.Send(connection, channel, data);
        if (sent) {
            sent_[Slot(data)] += data.size();
            if (hash_bytes_) {
                Mix(connection);
                for (const uint8_t byte : data) {
                    Mix(byte);
                }
            }
        }
        return sent;
    }
    void Disconnect(TransportConnection connection) override {
        inner_.Disconnect(connection);
    }
    TransportStats Stats() const noexcept override {
        return inner_.Stats();
    }

    uint64_t Sent(MessageKind kind) const {
        return sent_[static_cast<size_t>(kind)];
    }
    uint64_t Received(MessageKind kind) const {
        return received_[static_cast<size_t>(kind)];
    }
    uint64_t SentTotal() const {
        uint64_t total = 0;
        for (const uint64_t bytes : sent_) {
            total += bytes;
        }
        return total;
    }
    uint64_t ReceivedTotal() const {
        uint64_t total = 0;
        for (const uint64_t bytes : received_) {
            total += bytes;
        }
        return total;
    }
    uint64_t Hash() const {
        return hash_;
    }

private:
    static size_t Slot(std::span<const uint8_t> message) {
        MessageKind kind;
        return PeekKind(message, &kind) ? static_cast<size_t>(kind) : 0;
    }
    void Mix(uint64_t value) {
        hash_ = (hash_ ^ value) * 1099511628211ull;
    }

    ITransport& inner_;
    bool hash_bytes_;
    std::array<uint64_t, 6> sent_{};
    std::array<uint64_t, 6> received_{};
    uint64_t hash_ = 1469598103934665603ull;
};

/// The world: entities in one table per type and in the spatial grid, and the players' positions.
class World {
public:
    explicit World(const Scenario& scenario) : scenario_(scenario) {
        for (uint32_t type = 0; type < 4; ++type) {
            tables_[type] = std::make_unique<Table>(TableConfig{0, type});
        }
    }

    Table& TableOf(uint32_t type) {
        return *tables_[type];
    }

    void Build(SpatialGrid& grid) {
        ids_.reserve(scenario_.entities);
        x_.reserve(scenario_.entities);
        y_.reserve(scenario_.entities);
        z_.reserve(scenario_.entities);
        type_.reserve(scenario_.entities);
        for (uint32_t i = 0; i < scenario_.entities; ++i) {
            const uint32_t roll = static_cast<uint32_t>(rng_() % 100);
            const uint32_t type = roll < 60 ? 0 : roll < 85 ? 1 : roll < 95 ? 2 : 3;
            Table& table = *tables_[type];
            const EntityId id = table.Create();
            const auto where = table.Find(id);
            BenchColumns& columns = table.Columns(where->page);
            const float x = Random(0.0f, scenario_.world_size);
            const float y = Random(0.0f, 50.0f);
            const float z = Random(0.0f, scenario_.world_size);
            columns.x[where->slot] = x;
            columns.y[where->slot] = y;
            columns.z[where->slot] = z;
            columns.yaw[where->slot] = 1.0f;
            columns.health[where->slot] = static_cast<int32_t>(i % 1000);
            columns.flags[where->slot] = static_cast<uint16_t>(i);
            columns.state[where->slot] = static_cast<uint8_t>(i % 8);
            table.MarkDirty(id);
            ids_.push_back(id);
            x_.push_back(x);
            y_.push_back(y);
            z_.push_back(z);
            type_.push_back(type);
            grid.Insert(id, x, y, z);
        }
    }

    // One tick of change: every entity whose turn it is steps 0.2 m, back and forth along x, and one in ten
    // of those also changes its health.
    void ChangeSome(SpatialGrid& grid, uint64_t tick) {
        moves_.clear();
        const uint32_t period = scenario_.change_period;
        for (uint32_t i = static_cast<uint32_t>((period - tick % period) % period); i < scenario_.entities;
             i += period) {
            const uint64_t turn = (tick + i) / period;
            const float step = (turn & 1) ? -kEntityStep : kEntityStep;
            x_[i] = std::min(std::max(x_[i] + step, 0.0f), scenario_.world_size);
            Table& table = *tables_[type_[i]];
            const auto where = table.Find(ids_[i]);
            BenchColumns& columns = table.Columns(where->page);
            columns.x[where->slot] = x_[i];
            if (turn % 10 == 0) {
                columns.health[where->slot] = static_cast<int32_t>((turn * 7 + i) % 2000) - 1000;
            }
            table.MarkDirty(ids_[i]);
            moves_.push_back(PendingMove{ids_[i], x_[i], y_[i], z_[i]});
        }
        grid.ApplyMoves(moves_);
    }

    /// How many entities the last call of ChangeSome moved.
    size_t MovedLastTick() const {
        return moves_.size();
    }

    std::vector<Observer> PlaceObservers() {
        std::vector<Observer> eyes(scenario_.players);
        const float margin = scenario_.world_size / 10.0f;
        for (Observer& eye : eyes) {
            eye = Observer{Random(margin, scenario_.world_size - margin), 25.0f,
                           Random(margin, scenario_.world_size - margin)};
        }
        heading_.resize(eyes.size());
        step_.resize(eyes.size());
        for (size_t c = 0; c < eyes.size(); ++c) {
            heading_[c] = Random(0.0f, 2.0f * kPi);
            step_[c] = Random(0.0f, 1.0f) < 0.1f ? kFastStep : kWalkStep;
        }
        return eyes;
    }

    // Walkers and drivers keep a heading that turns a few degrees per tick and turn around at the edge.
    void StepObservers(std::vector<Observer>& eyes) {
        for (size_t c = 0; c < eyes.size(); ++c) {
            heading_[c] += Random(-0.05f, 0.05f);
            float nx = eyes[c].x + std::cos(heading_[c]) * step_[c];
            float nz = eyes[c].z + std::sin(heading_[c]) * step_[c];
            if (nx < 0.0f || nx > scenario_.world_size || nz < 0.0f || nz > scenario_.world_size) {
                heading_[c] += kPi;
                nx = std::min(std::max(nx, 0.0f), scenario_.world_size);
                nz = std::min(std::max(nz, 0.0f), scenario_.world_size);
            }
            eyes[c].x = nx;
            eyes[c].z = nz;
        }
    }

private:
    float Random(float low, float high) {
        return std::uniform_real_distribution<float>(low, high)(rng_);
    }

    Scenario scenario_;
    std::array<std::unique_ptr<Table>, 4> tables_;
    std::vector<EntityId> ids_;
    std::vector<float> x_, y_, z_;
    std::vector<uint32_t> type_;
    std::vector<float> heading_, step_;
    std::vector<PendingMove> moves_;
    std::mt19937 rng_{2024};
};

/// The cost of one tick, split by who pays it.
struct TickCost {
    double client_ms = 0.0;  // every client reading its transport
    double refresh_ms = 0.0; // the server quantizing what changed
    double update_ms = 0.0;  // the server's network phase: relevancy, replication, protocol and transport
};

ServerConfig BenchServerConfig(uint32_t players, bool budget) {
    ServerConfig config; // the pacing and the budget are the defaults of the server
    config.max_connections = players;
    config.max_visible_per_connection = kMaxVisible;
    config.tick_rate = kTickRate;
    if (!budget) {
        config.rate_kbps = 0; // only the pacing of the resends
    }
    return config;
}

/// A server, its clients, the network between them and the world they look at. `initial_clients` of 0 means
/// one for every player of the scenario.
class Sim {
public:
    Sim(const Scenario& scenario, uint32_t workers, const NetworkConditions& conditions, bool hash_bytes,
        bool budget = true, uint32_t initial_clients = 0)
        : scenario_(scenario), world_(scenario), grid_(GridConfig{128.0f, 128.0f, 128.0f}), jobs_(workers),
          network_(conditions), counted_(network_.Server(), hash_bytes), schema_(MakeSchema()) {
        world_.Build(grid_);
        eyes_ = world_.PlaceObservers();
        memory_before_ = bench::ResidentBytes();
        server_ =
            std::make_unique<ReplicationServer>(BenchServerConfig(scenario.players, budget), MakeRelevancy(),
                                                schema_, grid_, groups_, jobs_, counted_);
        server_->SetCallbacks(
            [this](ConnectionId id) {
                server_->Relevancy().SetObservers(id, std::span<const Observer>(&eyes_[id], 1));
            },
            [](ConnectionId) {});
        for (uint32_t i = 0; i < (initial_clients == 0 ? scenario.players : initial_clients); ++i) {
            AddClient();
        }
    }

    /// A new player connects. Returns its index among the clients.
    size_t AddClient() {
        transports_.push_back(network_.AddClient());
        clients_.push_back(std::make_unique<ClientReplica>(*transports_.back(), schema_));
        return clients_.size() - 1;
    }

    /// The share of the entities the connected clients can see for which they do not hold the server's
    /// current state (that includes those they have not received yet).
    double Divergence() {
        const uint32_t words = schema_.Find(0)->words;
        uint64_t visible = 0, current = 0;
        for (const auto& client : clients_) {
            if (client->State() != ClientState::Connected) {
                continue;
            }
            visible += server_->Relevancy().VisibleCount(client->ConnectionId());
            client->ForEachEntity([&](EntityId entity, const uint32_t* held) {
                const uint32_t* truth = server_->Replication().Store().Find(entity);
                bool same = truth != nullptr;
                for (uint32_t w = 0; same && w < words; ++w) {
                    same = held[w] == truth[w];
                }
                current += same ? 1 : 0;
            });
        }
        return visible == 0 ? 0.0 : 1.0 - static_cast<double>(current) / static_cast<double>(visible);
    }

    /// A client has received state for everything it knows it can see, and what it knows is nearly all that
    /// the server says it can see.
    bool HasEverything(size_t index) {
        const ClientReplica& client = *clients_[index];
        if (client.State() != ClientState::Connected || client.VisibleCount() == 0) {
            return false;
        }
        size_t with_state = 0;
        client.ForEachEntity([&](EntityId, const uint32_t*) { ++with_state; });
        const size_t visible = server_->Relevancy().VisibleCount(client.ConnectionId());
        return with_state == client.VisibleCount() && client.VisibleCount() * 100 >= visible * 98;
    }

    /// One tick. `changing` moves the world and the players; without it nothing changes.
    TickCost Tick(bool changing) {
        TickCost cost;
        network_.Advance(tick_);
        const auto t0 = Clock::now();
        for (const auto& client : clients_) {
            client->Poll();
        }
        const auto t1 = Clock::now();
        for (const auto& client : clients_) {
            if (client->State() == ClientState::Connected && client->LatestTick() > 0) {
                lag_sum_ += static_cast<double>(tick_ - client->LatestTick());
                ++lag_count_;
            }
        }
        if (changing) {
            world_.ChangeSome(grid_, tick_);
            world_.StepObservers(eyes_);
            for (const auto& client : clients_) {
                if (client->State() == ClientState::Connected) {
                    const ConnectionId id = client->ConnectionId();
                    server_->Relevancy().SetObservers(id, std::span<const Observer>(&eyes_[id], 1));
                }
            }
        }
        const auto t2 = Clock::now();
        for (uint32_t type = 0; type < 4; ++type) {
            server_->Refresh(world_.TableOf(type));
        }
        const auto t3 = Clock::now();
        server_->Update(tick_);
        const auto t4 = Clock::now();
        ++tick_;
        cost.client_ms = Ms(t0, t1);
        cost.refresh_ms = Ms(t2, t3);
        cost.update_ms = Ms(t3, t4);
        return cost;
    }

    bool AllConnected() const {
        for (const auto& client : clients_) {
            if (client->State() != ClientState::Connected) {
                return false;
            }
        }
        return true;
    }

    /// Every client sees what the relevancy engine says it can, with the state the server holds.
    bool Converged() {
        if (!AllConnected()) {
            return false;
        }
        const uint32_t words = schema_.Find(0)->words;
        for (const auto& client : clients_) {
            const ConnectionId id = client->ConnectionId();
            if (client->VisibleCount() != server_->Relevancy().VisibleCount(id)) {
                return false;
            }
            size_t with_state = 0;
            bool same = true;
            client->ForEachEntity([&](EntityId entity, const uint32_t* held) {
                ++with_state;
                const uint32_t* truth = server_->Replication().Store().Find(entity);
                if (truth == nullptr) {
                    same = false;
                    return;
                }
                for (uint32_t w = 0; w < words && same; ++w) {
                    same = held[w] == truth[w];
                }
            });
            if (!same || with_state != client->VisibleCount()) {
                return false;
            }
        }
        return true;
    }

    /// Plays ticks until every client has converged. Returns how many it took, or -1 if `limit` was not
    /// enough.
    int TicksToConverge(int limit, bool changing) {
        for (int i = 1; i <= limit; ++i) {
            Tick(changing);
            if (Converged()) {
                return i;
            }
        }
        return -1;
    }

    void SetLoss(double loss) {
        network_.SetConditions(Conditions(loss));
    }

    const CountingTransport& Counters() const {
        return counted_;
    }
    TransportStats NetworkStats() const {
        return counted_.Stats();
    }
    ReplicationServer& Server() {
        return *server_;
    }
    const std::vector<std::unique_ptr<ClientReplica>>& Clients() const {
        return clients_;
    }
    size_t MemoryGrowth() const {
        return bench::ResidentBytes() - memory_before_;
    }
    double MeanLag() const {
        return lag_count_ == 0 ? 0.0 : lag_sum_ / static_cast<double>(lag_count_);
    }
    uint64_t Tick() const {
        return tick_;
    }
    size_t MovedLastTick() const {
        return world_.MovedLastTick();
    }

private:
    Scenario scenario_;
    World world_;
    SpatialGrid grid_;
    RelevancyGroups groups_;
    ThreadPoolJobSystem jobs_;
    SimulatedNetwork network_;
    CountingTransport counted_;
    ReplicationSchema schema_;
    std::unique_ptr<ReplicationServer> server_;
    std::vector<ITransport*> transports_;
    std::vector<std::unique_ptr<ClientReplica>> clients_;
    std::vector<Observer> eyes_;
    uint64_t tick_ = 0;
    size_t memory_before_ = 0;
    double lag_sum_ = 0.0;
    uint64_t lag_count_ = 0;
};

struct Totals {
    uint64_t applied = 0, refused = 0, held = 0, dropped = 0, protocol_errors = 0, ignored = 0;
};

Totals ClientTotals(const Sim& sim) {
    Totals totals;
    for (const auto& client : sim.Clients()) {
        const ClientStats& stats = client->Stats();
        totals.applied += stats.buffers_applied;
        totals.refused += stats.buffers_refused;
        totals.held += stats.buffers_held;
        totals.dropped += stats.buffers_dropped;
        totals.protocol_errors += stats.protocol_errors + stats.malformed_messages;
        totals.ignored += stats.messages_ignored;
    }
    return totals;
}

struct Measurement {
    bool joined = false;  // all clients connected and converged on a clean network
    bool settled = false; // converged again after the world stopped, on the lossy network
    double refresh_ms = 0.0;
    double update_ms = 0.0;
#if TESSERA_PROFILE
    // Per tick, over every measured tick: the time of each stage of the server's Update and how many scopes ran.
    std::array<double, static_cast<size_t>(profile::Stage::Count)> stage_ms{};
    std::array<double, static_cast<size_t>(profile::Stage::Count)> stage_calls{};
    double update_mean_ms = 0.0; // the mean over the same ticks as the stages, not the median of the runs
    std::array<double, static_cast<size_t>(profile::Counter::Count)> counter_per_tick{};
#endif
    double client_poll_us = 0.0; // per client per tick
    double state_kbps = 0.0;     // per player, server to player
    double event_kbps = 0.0;
    double hello_kbps = 0.0;
    double down_kbps = 0.0;
    double ack_kbps = 0.0;     // per player, player to server
    double lost_percent = 0.0; // of the messages the server sent
    double visible_average = 0.0;
    double visible_most = 0.0;
    double mb_per_player = 0.0; // server and client of one player together
    double lag_ticks = 0.0;
    double records_per_player = 0.0; // records written per player per tick
    double steady_kbps = 0.0;        // per player, the last 5 seconds, with the saved-up burst used
    double peak_kbps = 0.0;          // per player, the busiest 10 seconds
    double divergence = 0.0;         // mean share of what a player sees that is not the current state
    double complete_share = 0.0;     // share of the records that carry every property
    double resend_wait_ticks = 0.0;  // mean ticks the server waits before an unconfirmed entity is sent again
    double moved_per_tick = 0.0;     // entities that changed per tick, over the measured ticks
    int settle_ticks = 0;
    Totals client_totals;
    uint64_t server_errors = 0;
};

Measurement Measure(const Scenario& scenario, uint32_t workers) {
    Measurement result;
    Sim sim(scenario, workers, Conditions(0.0), false);
    result.joined = sim.TicksToConverge(kJoinLimit, false) > 0;
    result.mb_per_player = static_cast<double>(sim.MemoryGrowth()) / scenario.players / (1024.0 * 1024.0);

    // From here on the network loses 5% of what it carries and the world and the players move.
    sim.SetLoss(0.05);
    for (uint32_t i = 0; i < kWarmupTicks; ++i) {
        sim.Tick(true);
    }

    const CountingTransport& counted = sim.Counters();
    const uint64_t state_before = counted.Sent(MessageKind::StateBuffer);
    const uint64_t event_before = counted.Sent(MessageKind::EventBatch);
    const uint64_t hello_before = counted.Sent(MessageKind::ServerHello);
    const uint64_t sent_before = counted.SentTotal();
    const uint64_t ack_before = counted.Received(MessageKind::Ack);
    const TransportStats net_before = sim.NetworkStats();
    const Totals clients_before = ClientTotals(sim);

    std::vector<double> refresh_runs, update_runs, client_runs;
#if TESSERA_PROFILE
    profile::Reset(); // the joining and the warm-up are not part of the table
#endif
    uint64_t records = 0, complete_records = 0, moved = 0;
    std::vector<uint64_t> bytes_per_tick;
    uint64_t sent_so_far = counted.SentTotal();
    double divergence_sum = 0.0;
    int divergence_samples = 0;
    int ticks_done = 0;
    for (int run = 0; run < kRuns; ++run) {
        TickCost sum;
        for (int i = 0; i < kTicksPerRun; ++i) {
            const TickCost cost = sim.Tick(true);
            records += sim.Server().Replication().LastStats().records;
            complete_records += sim.Server().Replication().LastStats().complete_records;
            moved += sim.MovedLastTick();
            bytes_per_tick.push_back(counted.SentTotal() - sent_so_far);
            sent_so_far = counted.SentTotal();
            if (++ticks_done % 5 == 0) { // not every tick: it looks at every entity of every player
                divergence_sum += sim.Divergence();
                ++divergence_samples;
            }
            sum.client_ms += cost.client_ms;
            sum.refresh_ms += cost.refresh_ms;
            sum.update_ms += cost.update_ms;
        }
        refresh_runs.push_back(sum.refresh_ms / kTicksPerRun);
        update_runs.push_back(sum.update_ms / kTicksPerRun);
        client_runs.push_back(sum.client_ms / kTicksPerRun / scenario.players * 1000.0);
    }
    result.records_per_player = static_cast<double>(records) / (kRuns * kTicksPerRun) / scenario.players;
    result.moved_per_tick = static_cast<double>(moved) / (kRuns * kTicksPerRun);
    result.complete_share =
        records == 0 ? 0.0 : static_cast<double>(complete_records) / static_cast<double>(records);
    result.divergence = divergence_samples == 0 ? 0.0 : divergence_sum / divergence_samples;
    {
        const auto window_kbps = [&](size_t first, size_t count) {
            uint64_t sum = 0;
            for (size_t i = first; i < first + count; ++i) {
                sum += bytes_per_tick[i];
            }
            return static_cast<double>(sum) * 8.0 / 1000.0 / (static_cast<double>(count) / kTickRate) /
                   scenario.players;
        };
        for (size_t first = 0; first + kWindowTicks <= bytes_per_tick.size(); ++first) {
            result.peak_kbps = std::max(result.peak_kbps, window_kbps(first, kWindowTicks));
        }
        result.steady_kbps = window_kbps(bytes_per_tick.size() - kSteadyTicks, kSteadyTicks);
        double wait = 0.0;
        for (const auto& client : sim.Clients()) {
            wait += sim.Server().Replication().ResendAfterTicks(client->ConnectionId());
        }
        result.resend_wait_ticks = wait / scenario.players;
    }
    result.refresh_ms = Median(refresh_runs);
    result.update_ms = Median(update_runs);
#if TESSERA_PROFILE
    for (size_t i = 0; i < result.stage_ms.size(); ++i) {
        const profile::StageTotals totals = profile::Totals(static_cast<profile::Stage>(i));
        result.stage_ms[i] = static_cast<double>(totals.nanoseconds) / 1e6 / (kRuns * kTicksPerRun);
        result.stage_calls[i] = static_cast<double>(totals.calls) / (kRuns * kTicksPerRun);
    }
    for (size_t i = 0; i < result.counter_per_tick.size(); ++i) {
        result.counter_per_tick[i] = static_cast<double>(profile::Value(static_cast<profile::Counter>(i))) /
                                     (kRuns * kTicksPerRun);
    }
    for (const double run : update_runs) { // every run has the same number of ticks
        result.update_mean_ms += run / static_cast<double>(update_runs.size());
    }
#endif
    result.client_poll_us = Median(client_runs);

    const double seconds = static_cast<double>(kRuns * kTicksPerRun) / kTickRate;
    const auto kbps = [&](uint64_t bytes) {
        return static_cast<double>(bytes) * 8.0 / 1000.0 / seconds / scenario.players;
    };
    result.state_kbps = kbps(counted.Sent(MessageKind::StateBuffer) - state_before);
    result.event_kbps = kbps(counted.Sent(MessageKind::EventBatch) - event_before);
    result.hello_kbps = kbps(counted.Sent(MessageKind::ServerHello) - hello_before);
    result.down_kbps = kbps(counted.SentTotal() - sent_before);
    result.ack_kbps = kbps(counted.Received(MessageKind::Ack) - ack_before);
    const TransportStats net_after = sim.NetworkStats();
    const uint64_t messages = net_after.messages_sent - net_before.messages_sent;
    result.lost_percent =
        messages == 0 ? 0.0
                      : 100.0 * static_cast<double>(net_after.messages_lost - net_before.messages_lost) /
                            static_cast<double>(messages);
    result.lag_ticks = sim.MeanLag();

    double visible_sum = 0.0;
    for (const auto& client : sim.Clients()) {
        const double visible =
            static_cast<double>(sim.Server().Relevancy().VisibleCount(client->ConnectionId()));
        visible_sum += visible;
        result.visible_most = std::max(result.visible_most, visible);
    }
    result.visible_average = visible_sum / scenario.players;

    const Totals clients_after = ClientTotals(sim);
    result.client_totals.applied = clients_after.applied - clients_before.applied;
    result.client_totals.refused = clients_after.refused - clients_before.refused;
    result.client_totals.held = clients_after.held - clients_before.held;
    result.client_totals.dropped = clients_after.dropped - clients_before.dropped;
    result.client_totals.protocol_errors = clients_after.protocol_errors;
    result.server_errors = sim.Server().Stats().protocol_errors + sim.Server().Stats().malformed_messages;

    // The world stops. On the same lossy network, how long until every player sees exactly what the server
    // has?
    result.settle_ticks = sim.TicksToConverge(kSettleLimit, false);
    result.settled = result.settle_ticks > 0;
    return result;
}

/// Records written per change when only the pacing of the resends is on, in the main scenario with one
/// thread.
struct PacingResult {
    double records_per_change;
    double divergence; // share of what a player sees that is not the current state
};

PacingResult PacingOnly() {
    Sim sim(kMain, 1, Conditions(0.0), false, /*budget=*/false);
    sim.TicksToConverge(kJoinLimit, false);
    sim.SetLoss(0.05);
    for (uint32_t i = 0; i < kWarmupTicks; ++i) {
        sim.Tick(true);
    }
    constexpr int kTicks = 40;
    uint64_t records = 0;
    double divergence = 0.0;
    int samples = 0;
    for (int i = 0; i < kTicks; ++i) {
        sim.Tick(true);
        records += sim.Server().Replication().LastStats().records;
        if (i % 5 == 0) {
            divergence += sim.Divergence();
            ++samples;
        }
    }
    double visible = 0.0;
    for (const auto& client : sim.Clients()) {
        visible += static_cast<double>(sim.Server().Relevancy().VisibleCount(client->ConnectionId()));
    }
    visible /= kMain.players;
    const double changes_per_tick = visible / kMain.change_period; // each entity changes once per period
    return PacingResult{static_cast<double>(records) / kTicks / kMain.players / changes_per_tick,
                        divergence / samples};
}

/// A player joins the main scenario while it runs, on the lossy network. Returns the ticks until it holds
/// state for everything it can see, or -1.
int JoinTicksIntoARunningWorld() {
    Sim sim(kMain, 1, Conditions(0.0), false, /*budget=*/true, kMain.players - 1);
    sim.TicksToConverge(kJoinLimit, false);
    sim.SetLoss(0.05);
    for (uint32_t i = 0; i < kWarmupTicks + 120; ++i) { // long enough for the others to use up their savings
        sim.Tick(true);
    }
    const size_t newcomer = sim.AddClient();
    for (int i = 1; i <= kJoinLimit; ++i) {
        sim.Tick(true);
        if (sim.HasEverything(newcomer)) {
            return i;
        }
    }
    return -1;
}

/// A small world on the lossy network from the start, run for 40 ticks; returns a hash of every byte the
/// server sent, in order.
uint64_t SentBytesHash(uint32_t workers) {
    Sim sim(kSmall, workers, Conditions(0.05), true);
    for (int i = 0; i < 40; ++i) {
        sim.Tick(true);
    }
    return sim.Counters().Hash();
}

/// Ticks for a small group to join a lossy network and converge, for information.
int JoinTicksUnderLoss() {
    Sim sim(kSmall, 1, Conditions(0.05), false);
    return sim.TicksToConverge(kJoinLimit, false);
}

/// A scenario like `base` in which each entity changes once every `period` ticks.
constexpr Scenario WithChangePeriod(const Scenario& base, uint32_t period) noexcept {
    return Scenario{base.players, base.entities, base.world_size, period};
}

// The change periods of the curve, which move about 1, 1,000, 3,000, 10,000 and 33,333 entities per tick in
// the main scenario. The first is almost still, so that the fixed cost is measured, not extrapolated; the
// last one is the main scenario itself.
constexpr uint32_t kCurvePeriods[5] = {200000, 200, 67, 20, 6};
constexpr size_t kCurvePoints = std::size(kCurvePeriods);
static_assert(kCurvePeriods[kCurvePoints - 1] == kMain.change_period,
              "the last point of the curve is the main scenario");
constexpr uint32_t kCurveSteadyTicks = 40; // the ticks of the byte comparison, after the warm-up

/// A hash of every byte the server sends in a full-size scenario: players join on a clean network, then the
/// network starts to lose 5% and the world moves for the warm-up and 40 more ticks. It is a run of its own,
/// so that hashing does not enter the time of the measured ones.
uint64_t PointBytesHash(const Scenario& scenario, uint32_t workers) {
    Sim sim(scenario, workers, Conditions(0.0), true);
    sim.TicksToConverge(kJoinLimit, false);
    sim.SetLoss(0.05);
    for (uint32_t i = 0; i < kWarmupTicks + kCurveSteadyTicks; ++i) {
        sim.Tick(true);
    }
    return sim.Counters().Hash();
}

void PrintCurveRow(uint32_t period, uint32_t workers, const Measurement& m) {
    std::printf("%6u  %11.1f  %7u  %8.2f  %13.2f  %9.2f  %15.2f  %19.2f  %13.2f\n", period, m.moved_per_tick,
                workers, m.refresh_ms + m.update_ms, m.refresh_ms, m.update_ms, m.steady_kbps,
                m.records_per_player, 100.0 * m.divergence);
}

void PrintScenario(const Measurement& m, uint32_t workers) {
    std::printf("  %u worker%s: network phase %.2f ms (quantizing %.2f ms, update %.2f ms); clients %.1f us "
                "each; %.0f kbps per player down, %.0f up\n",
                workers, workers == 1 ? "" : "s", m.refresh_ms + m.update_ms, m.refresh_ms, m.update_ms,
                m.client_poll_us, m.down_kbps, m.ack_kbps);
}

#if TESSERA_PROFILE
/// The time of each stage of the server's Update, per tick, for one run of the scenario. The stages on the
/// main thread add up to the update (what is left over is listed); those on the workers (marked cpu) are
/// summed over the workers, so with several workers they exceed the time that passed.
void PrintStageTable(const Measurement& m, uint32_t workers) {
    using profile::Stage;
    char name[120];
    const char* kind = workers == 1 ? "1 thread" : "8 workers";
    double main_thread_ms = 0.0;
    double calls = 0.0;
    for (size_t i = 0; i < m.stage_ms.size(); ++i) {
        const Stage stage = static_cast<Stage>(i);
        const bool cpu = profile::PerConnection(stage);
        std::snprintf(name, sizeof(name), "network stage, %s: %s (%s)", kind, profile::Name(stage),
                      cpu ? "cpu, summed over workers" : "wall");
        Info(name, m.stage_ms[i], "ms");
        std::snprintf(name, sizeof(name), "network stage, %s: %s, scopes per tick", kind,
                      profile::Name(stage));
        Info(name, m.stage_calls[i], "calls");
        calls += m.stage_calls[i];
        if (!cpu && !profile::Nested(stage)) { // the nested ones are inside transport or relevancy
            main_thread_ms += m.stage_ms[i];
        }
    }
    // Inside the relevancy: what the three steps on the main thread leave out, and, with one thread, what the
    // per-connection stages leave out of the region (with several workers those stages are summed CPU time).
    const auto ms = [&](Stage stage) { return m.stage_ms[static_cast<size_t>(stage)]; };
    std::snprintf(name, sizeof(name), "network relevancy not accounted for, %s", kind);
    Info(name, ms(Stage::Relevancy) - ms(Stage::RelFold) - ms(Stage::RelIndex) - ms(Stage::RelRegion), "ms");
    if (workers == 1) {
        const double per_connection = ms(Stage::RelDestroy) + ms(Stage::RelFull) + ms(Stage::RelGather) +
                                      ms(Stage::RelSortUnique) + ms(Stage::RelFilter) + ms(Stage::RelFinish);
        Info("network relevancy region not accounted for, 1 thread", ms(Stage::RelRegion) - per_connection,
             "ms");
    }
    using profile::Counter;
    const auto count = [&](Counter counter) { return m.counter_per_tick[static_cast<size_t>(counter)]; };
    std::snprintf(name, sizeof(name), "network relevancy: changes per tick, %s", kind);
    Info(name, count(Counter::Changes), "ents");
    std::snprintf(name, sizeof(name), "network relevancy: candidates looked at by the gather per tick, %s",
                  kind);
    Info(name, count(Counter::Candidates), "ents");
    std::snprintf(name, sizeof(name), "network relevancy: changes sorted per tick, %s", kind);
    Info(name, count(Counter::Sorted), "ents");
    std::snprintf(name, sizeof(name), "network relevancy: near per tick, %s", kind);
    Info(name, count(Counter::Near), "ents");
    std::snprintf(name, sizeof(name), "network relevancy: changes sorted per connection per tick, %s", kind);
    Info(name, count(Counter::Sorted) / kMain.players, "ents");
    std::snprintf(name, sizeof(name), "network relevancy: candidates looked at for each one sorted, %s", kind);
    Info(name, count(Counter::Sorted) > 0 ? count(Counter::Candidates) / count(Counter::Sorted) : 0.0, "x");
    std::snprintf(name, sizeof(name), "network relevancy: sorted for each one that is near, %s", kind);
    Info(name, count(Counter::Near) > 0 ? count(Counter::Sorted) / count(Counter::Near) : 0.0, "x");
    std::snprintf(name, sizeof(name), "network stages on the main thread, %s: sum", kind);
    Info(name, main_thread_ms, "ms");
    std::snprintf(name, sizeof(name), "network update not accounted for, %s", kind);
    Info(name, m.update_mean_ms - main_thread_ms, "ms");
    const double timer_ns = profile::TimerCostNanoseconds();
    Info("network stage timer, cost of one scope", timer_ns, "ns");
    std::snprintf(name, sizeof(name),
                  "network stage timers, estimated cost per tick (every scope times one scope's cost), %s",
                  kind);
    Info(name, calls * timer_ns / 1e6, "ms");
}
#endif

} // namespace

void CheckNetwork() {
    std::printf("network: %u players, %u entities in a %.0f m world, %u-tick change period, 5%% loss, "
                "1 to 2 ticks of delay each way\n",
                kMain.players, kMain.entities, kMain.world_size, kMain.change_period);
    std::printf("network goals: 3.3 ms per tick with 1 thread (not met yet), 1 ms with 8 workers (not met "
                "yet), 100 kbps per "
                "player on average and 200 over 10 seconds (the budget)\n");
    const Measurement one = Measure(kMain, 1);
    PrintScenario(one, 1);
    const Measurement eight = Measure(kMain, 8);
    PrintScenario(eight, 8);
#if TESSERA_PROFILE
    PrintStageTable(one, 1);
    PrintStageTable(eight, 8);
#else
    std::printf("network stage table: build with -DTESSERA_PROFILE=ON (preset windows-msvc-profile)\n");
#endif

    ReportBool("network clients converge after joining", one.joined && eight.joined);
    ReportBool("network clients converge after the world stops", one.settled && eight.settled);
    Info("network ticks to converge after the world stops", one.settle_ticks, "ticks");
    Info("network visible per player (mean)", one.visible_average, "ents");
    Info("network visible per player (most)", one.visible_most, "ents");
    Report("network phase, 1 thread, mean per tick", one.refresh_ms + one.update_ms, "ms", kLimitOneThreadMs);
    Info("  of which quantizing what changed", one.refresh_ms, "ms");
    Info("  of which relevancy, replication, protocol", one.update_ms, "ms");
    Report("network phase, 8 workers, mean per tick", eight.refresh_ms + eight.update_ms, "ms",
           kLimitEightWorkersMs);
    Report("network bandwidth per player, last 5 s", one.steady_kbps, "kbps", kLimitSteadyKbps);
    Report("network bandwidth per player, busiest 10 s", one.peak_kbps, "kbps", kLimitPeakKbps);
    Info("network bandwidth per player, whole run", one.down_kbps, "kbps");
    Info("network state a player holds that is not current", 100.0 * one.divergence, "%");
    Info("network complete records, share of all", 100.0 * one.complete_share, "%");
    Info("network resend wait the server settled on", one.resend_wait_ticks, "ticks");
    Info("  of which state buffers", one.state_kbps, "kbps");
    Info("  of which event batches", one.event_kbps, "kbps");
    Info("  of which hellos", one.hello_kbps, "kbps");
    Info("network acks per player, player to server", one.ack_kbps, "kbps");
    Info("network records written per player per tick", one.records_per_player, "rec");
    Info("network messages the network lost", one.lost_percent, "%");
    Info("network state lag of a client (mean)", one.lag_ticks, "ticks");
    Info("network state buffers applied per client, per tick",
         static_cast<double>(one.client_totals.applied) / kMain.players / (kRuns * kTicksPerRun), "bufs");
    Info("network state buffers refused or held",
         static_cast<double>(one.client_totals.refused + one.client_totals.held), "bufs");
    Info("network state buffers dropped", static_cast<double>(one.client_totals.dropped), "bufs");
    Report("network client poll, per client per tick", one.client_poll_us, "us", kLimitClientPollUs);
    Report("network memory per player (server + client)", one.mb_per_player, "MB", kLimitMbPerPlayer);
    ReportBool("network bytes sent, 1 vs 8 workers, 40 ticks", SentBytesHash(1) == SentBytesHash(8));
    const PacingResult pacing = PacingOnly();
    Report("network records per change, pacing only", pacing.records_per_change, "x", kLimitResendFactor);
    Info("network state not current, no budget (pacing only)", 100.0 * pacing.divergence, "%");
    const int join = JoinTicksIntoARunningWorld();
    Report("network a new player receives everything", join < 0 ? 1e9 : join, "ticks", kLimitJoinTicks);
    Info("network join under 5% loss (16 players)", JoinTicksUnderLoss(), "ticks");
}

void CheckNetworkCurve(bool reverse) {
    std::array<uint32_t, kCurvePoints> periods{};
    for (size_t i = 0; i < periods.size(); ++i) {
        periods[i] = kCurvePeriods[reverse ? periods.size() - 1 - i : i];
    }
    std::printf("network curve: %u players, %u entities, %.0f m world, 5%% loss, periods", kMain.players,
                kMain.entities, kMain.world_size);
    for (const uint32_t period : periods) {
        std::printf(" %u", period);
    }
    std::printf("\n");
    Info("network curve: WriteRecord indicator, before the points", WriteRecordIndicatorNs(), "ns");
    std::printf("%6s  %11s  %7s  %8s  %13s  %9s  %15s  %19s  %13s\n", "period", "moved/tick", "workers",
                "phase ms", "quantizing ms", "update ms", "kbps (last 5 s)", "records/player/tick",
                "not current %");

    // The fit and the residuals are kept in the order of the periods, whatever order they run in.
    std::array<double, kCurvePoints> moved_of{};
    std::array<std::array<double, kCurvePoints>, 2> phase_of{};
    char name[120];
    for (const uint32_t period : periods) {
        const size_t slot =
            static_cast<size_t>(std::find(std::begin(kCurvePeriods), std::end(kCurvePeriods), period) -
                                std::begin(kCurvePeriods));
        const Scenario scenario = WithChangePeriod(kMain, period);
        const Measurement one = Measure(scenario, 1);
        const Measurement eight = Measure(scenario, 8);
        PrintCurveRow(period, 1, one);
        PrintCurveRow(period, 8, eight);
        moved_of[slot] = one.moved_per_tick;
        phase_of[0][slot] = one.refresh_ms + one.update_ms;
        phase_of[1][slot] = eight.refresh_ms + eight.update_ms;

        std::snprintf(name, sizeof(name), "network curve, period %u: clients converge", period);
        ReportBool(name, one.joined && one.settled && eight.joined && eight.settled);
        std::snprintf(name, sizeof(name), "network curve, period %u: bytes sent, 1 vs 8 workers", period);
        const uint64_t hash_one = PointBytesHash(scenario, 1);
        const uint64_t hash_eight = PointBytesHash(scenario, 8);
        ReportBool(name, hash_one == hash_eight);
    }
    Info("network curve: WriteRecord indicator, after the points", WriteRecordIndicatorNs(), "ns");

    // y = a + b * x, with x the entities that moved per tick (measured, not nominal) and y the phase in ms.
    const std::vector<double> x(moved_of.begin(), moved_of.end());
    bool linear = true;
    for (size_t k = 0; k < 2; ++k) {
        const char* kind = k == 0 ? "1 thread" : "8 workers";
        const std::vector<double> y(phase_of[k].begin(), phase_of[k].end());
        const bench::LineFit fit = bench::FitLine(x, y);
        std::snprintf(name, sizeof(name), "network curve fit, %s: fixed cost a", kind);
        Info(name, fit.intercept, "ms");
        std::snprintf(name, sizeof(name), "network curve fit, %s: cost per moved entity b", kind);
        Info(name, fit.slope * 1000.0, "us"); // ms per entity to microseconds
        std::snprintf(name, sizeof(name), "network curve fit, %s: R squared", kind);
        Info(name, fit.r2, "");
        for (size_t i = 0; i < std::size(kCurvePeriods); ++i) {
            const double residual = y[i] - (fit.intercept + fit.slope * x[i]);
            std::snprintf(name, sizeof(name), "network curve fit, %s: residual at period %u", kind,
                          kCurvePeriods[i]);
            Info(name, residual, "ms");
            if (std::abs(residual) > 0.10 * y[i]) {
                linear = false;
            }
        }
    }
    std::printf("network curve: %s\n", linear ? "linear within 10%" : "not linear");
}
