// SPDX-License-Identifier: Apache-2.0
// A whole small game over a simulated network: a server with the real relevancy and replication engines, real
// clients, entities that move, are created and destroyed, players that walk, and a network that loses,
// repeats and delays messages.
#pragma once

#include "server_test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

namespace network_test {

struct NetParams {
    uint32_t seed = 1;
    uint32_t workers = 1;
    uint32_t clients = 4;
    uint32_t entities = 60;
    uint32_t ticks = 80;
    NetworkConditions conditions = [] {
        NetworkConditions c;
        c.loss = 0.2;
        c.duplicate = 0.1;
        c.min_delay_ticks = 0;
        c.max_delay_ticks = 3;
        return c;
    }();
    double destroy = 0.02;      // per entity and tick: it is destroyed and another appears
    uint32_t settle_every = 20; // mutate and lose messages this many ticks, then stop and check
    bool few_values = false; // values flip between a couple of states, so entities often return to old ones
    float area = 300.0f;     // players and entities live in a square this many metres wide
    double noise = 0.0;      // per tick: chance that garbage is injected on the unreliable channel, both ways
};

class NetWorld {
public:
    explicit NetWorld(const NetParams& params)
        : params_(params), jobs_(params.workers), network_(Seeded(params)),
          server_(MakeServerConfig(params), TestRelevancyConfig(), schema_, grid_, groups_, jobs_,
                  network_.Server()),
          table_(TableConfig{0, kMobType}), random_(params.seed * 7919u + 1u) {
        server_.SetCallbacks(
            [this](ConnectionId id) {
                ++connects_;
                Place(id);
            },
            [this](ConnectionId) { ++disconnects_; });
        for (uint32_t i = 0; i < params.entities; ++i) {
            mobs_.push_back(Spawn());
        }
        for (uint32_t i = 0; i < params.clients; ++i) {
            AddClient();
        }
    }

    size_t AddClient() {
        transports_.push_back(network_.AddClient());
        clients_.push_back(std::make_unique<ClientReplica>(*transports_.back(), schema_));
        return clients_.size() - 1;
    }
    void DropClient(size_t index) {
        clients_[index]->Disconnect();
    }

    // The whole run: cycles of messy play followed by a quiet stretch on a clean channel, with a check after
    // each.
    void Run() {
        if (params_.settle_every > 0) {
            for (uint32_t done = 0; done + params_.settle_every <= params_.ticks;
                 done += params_.settle_every) {
                Play(params_.settle_every);
                Settle(30);
                converged_ = Check() && converged_;
            }
        } else {
            Play(params_.ticks);
        }
        Settle(40);
        converged_ = Check() && converged_;
    }

    // Plays `count` ticks with the world changing and the network as it is.
    void Play(uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            Tick(true);
        }
    }
    // Plays `count` ticks with nothing changing and no message lost or repeated (delays stay).
    void Settle(uint32_t count) {
        NetworkConditions clean = params_.conditions;
        clean.loss = 0.0;
        clean.duplicate = 0.0;
        network_.SetConditions(clean);
        for (uint32_t i = 0; i < count; ++i) {
            Tick(false);
        }
        network_.SetConditions(params_.conditions);
    }

    // Every connected client sees exactly what the relevancy engine says it can see, with the server's state.
    bool Check() {
        bool ok = true;
        for (const auto& client : clients_) {
            if (client->State() != ClientState::Connected) {
                continue; // a client that was dropped
            }
            const ConnectionId connection = client->ConnectionId();
            ok = ok && server_.Relevancy().VisibleCount(connection) == client->VisibleCount();
            for (const EntityId mob : mobs_) {
                const bool visible = server_.Relevancy().CompactIdOf(connection, mob) != kInvalidCompactId;
                const uint32_t* held = client->WordsOf(mob);
                const uint32_t* truth = server_.Replication().Store().Find(mob);
                if (visible) {
                    ok = ok && SameWords(held, truth) && held != nullptr;
                } else {
                    ok = ok && held == nullptr;
                }
            }
            ++checks_;
        }
        return ok;
    }

    bool AllConnected() const {
        return std::all_of(clients_.begin(), clients_.end(),
                           [](const auto& client) { return client->State() == ClientState::Connected; });
    }

    // A fingerprint of everything the clients hold and of what the server sent.
    uint64_t Fingerprint() {
        uint64_t hash = 0xCBF29CE484222325ull;
        auto mix = [&](uint64_t value) {
            for (int i = 0; i < 8; ++i) {
                hash = (hash ^ ((value >> (8 * i)) & 0xFF)) * 0x100000001B3ull;
            }
        };
        for (const auto& client : clients_) {
            mix(client->VisibleCount());
            mix(client->LatestTick());
            for (const EntityId mob : mobs_) {
                const uint32_t* words = client->WordsOf(mob);
                mix(words == nullptr ? 0xFFFFFFFFull : 1);
                for (int w = 0; words != nullptr && w < 9; ++w) {
                    mix(words[w]);
                }
            }
        }
        mix(server_.Stats().state_buffers_sent);
        mix(server_.Stats().event_batches_sent);
        mix(network_.Server().Stats().bytes_sent);
        mix(network_.Server().Stats().messages_lost);
        return hash;
    }

    bool converged() const {
        return converged_;
    }
    uint64_t checks() const {
        return checks_;
    }
    ReplicationServer& server() {
        return server_;
    }
    SimulatedNetwork& network() {
        return network_;
    }
    ClientReplica& client(size_t index) {
        return *clients_[index];
    }
    size_t client_count() const {
        return clients_.size();
    }
    uint64_t connects() const {
        return connects_;
    }
    uint64_t disconnects() const {
        return disconnects_;
    }
    uint64_t tick() const {
        return tick_;
    }
    const std::vector<EntityId>& mobs() const {
        return mobs_;
    }

private:
    static NetworkConditions Seeded(const NetParams& params) {
        NetworkConditions conditions = params.conditions;
        conditions.seed = params.seed;
        return conditions;
    }
    static ServerConfig MakeServerConfig(const NetParams& params) {
        ServerConfig config;
        config.max_connections = std::max<uint32_t>(params.clients + 4, 8);
        config.max_visible_per_connection = 1000;
        return config;
    }

    double Chance01() {
        return static_cast<double>(random_() >> 11) * (1.0 / 9007199254740992.0);
    }
    bool Chance(double probability) {
        return Chance01() < probability;
    }
    float Between(float low, float high) {
        return low + static_cast<float>(Chance01()) * (high - low);
    }
    uint32_t Roll(uint32_t below) {
        return static_cast<uint32_t>(random_() % below);
    }

    // A new player gets a place in the world.
    void Place(ConnectionId id) {
        if (observers_.size() <= id) {
            observers_.resize(id + 1, Observer{0.0f, 0.0f, 0.0f});
        }
        observers_[id] = params_.few_values
                             ? Observer{100.0f, 0.0f, 0.0f}
                             : Observer{Between(0.0f, params_.area), 0.0f, Between(0.0f, params_.area)};
        server_.Relevancy().SetObservers(id, std::span<const Observer>(&observers_[id], 1));
    }

    EntityId Spawn() {
        const EntityId id = table_.Create();
        Randomize(id);
        grid_.Insert(id, X(id), 0.0f, Z(id));
        return id;
    }

    float X(EntityId id) {
        const auto where = table_.Find(id);
        return table_.Columns(where->page).x[where->slot];
    }
    float Z(EntityId id) {
        const auto where = table_.Find(id);
        return table_.Columns(where->page).z[where->slot];
    }

    // New values for an entity (position included); the caller tells the grid.
    void Randomize(EntityId id) {
        const auto where = table_.Find(id);
        TestColumns& c = table_.Columns(where->page);
        const uint32_t slot = where->slot;
        if (params_.few_values) {
            c.x[slot] = Chance(0.5) ? 100.0f : 100.5f;
            c.z[slot] = 0.0f;
            c.yaw[slot] = Chance(0.5) ? 0.0f : 1.0f;
            c.health[slot] = Chance(0.5) ? 100 : 99;
            c.flags[slot] = Chance(0.5) ? 0 : 1;
            c.state[slot] = static_cast<uint8_t>(Roll(2));
        } else {
            c.x[slot] = Between(0.0f, params_.area);
            c.z[slot] = Between(0.0f, params_.area);
            c.yaw[slot] = Between(-7.0f, 7.0f);
            c.health[slot] = static_cast<int32_t>(Roll(2200)) - 1100;
            c.flags[slot] = static_cast<uint16_t>(Roll(65536));
            c.state[slot] = static_cast<uint8_t>(Roll(10));
        }
        table_.MarkDirty(id);
    }

    void Change(EntityId id) {
        const float old_x = X(id), old_z = Z(id);
        if (params_.few_values) {
            Randomize(id);
        } else {
            const auto where = table_.Find(id);
            TestColumns& c = table_.Columns(where->page);
            c.x[where->slot] = std::clamp(old_x + Between(-6.0f, 6.0f), 0.0f, params_.area);
            c.z[where->slot] = std::clamp(old_z + Between(-6.0f, 6.0f), 0.0f, params_.area);
            if (Chance(0.3)) {
                c.health[where->slot] = static_cast<int32_t>(Roll(2200)) - 1100;
            }
            table_.MarkDirty(id);
        }
        pending_moves_.push_back(PendingMove{id, X(id), 0.0f, Z(id)});
    }

    void MoveWorld() {
        for (size_t i = 0; i < mobs_.size(); ++i) {
            if (Chance(params_.destroy)) {
                grid_.Remove(mobs_[i]);
                table_.Destroy(mobs_[i]);
                server_.Relevancy().OnEntityDestroyed(mobs_[i]);
                mobs_[i] = Spawn();
            } else if (Chance(0.3)) {
                Change(mobs_[i]);
            }
        }
        for (const auto& client : clients_) {
            if (client->State() != ClientState::Connected) {
                continue;
            }
            Observer& eye = observers_[client->ConnectionId()];
            if (!params_.few_values) {
                eye.x = std::clamp(eye.x + Between(-8.0f, 8.0f), 0.0f, params_.area);
                eye.z = std::clamp(eye.z + Between(-8.0f, 8.0f), 0.0f, params_.area);
            }
            server_.Relevancy().SetObservers(client->ConnectionId(), std::span<const Observer>(&eye, 1));
        }
    }

    // Garbage that is only dropped. A forged message that happens to be well formed (a state buffer whose
    // body is an empty buffer, an ack with a made-up tick) cannot be told from a real one: protecting against
    // that is the transport's job (ENet checks its packets), so none of it is injected here.
    void InjectGarbage() {
        const size_t target = Roll(static_cast<uint32_t>(transports_.size()));
        std::vector<uint8_t> noise(1 + Roll(40));
        for (uint8_t& b : noise) {
            b = static_cast<uint8_t>(random_());
        }
        noise[0] = Chance(0.5) ? 0 : 255; // not a message kind
        // Toward the server from a client, and toward a client from the server: only on the unreliable
        // channel, which is where garbage is merely dropped.
        transports_[target]->Send(0, Channel::Unreliable, noise);
        network_.Server().Send(static_cast<TransportConnection>(target + 1), Channel::Unreliable, noise);
        // A state buffer that cites an event batch that will never come: the client holds it and, when too
        // many wait, drops the oldest.
        std::vector<uint8_t> bytes;
        EncodeStateBuffer(StateBufferHeader{tick_, 1'000'000'000ull}, noise, bytes);
        network_.Server().Send(static_cast<TransportConnection>(target + 1), Channel::Unreliable, bytes);
        // A cut-short ack and a cut-short state buffer header.
        std::vector<uint8_t> ack;
        Encode(Ack{tick_, 0xFFFFFFFFu}, ack);
        ack.resize(ack.size() / 2);
        transports_[target]->Send(0, Channel::Unreliable, ack);
        network_.Server().Send(static_cast<TransportConnection>(target + 1), Channel::Unreliable,
                               std::vector<uint8_t>{static_cast<uint8_t>(MessageKind::StateBuffer)});
    }

    void Tick(bool mutate) {
        network_.Advance(tick_);
        for (const auto& client : clients_) {
            client->Poll();
        }
        if (mutate) {
            MoveWorld();
        }
        grid_.ApplyMoves(pending_moves_);
        pending_moves_.clear();
        server_.Refresh(table_);
        server_.Update(tick_);
        if (mutate && params_.noise > 0.0 && Chance(params_.noise)) {
            InjectGarbage();
        }
        ++tick_;
    }

    NetParams params_;
    ReplicationSchema schema_ = MakeTestSchema();
    SpatialGrid grid_{GridConfig{64.0f, 64.0f, 64.0f}};
    RelevancyGroups groups_;
    ThreadPoolJobSystem jobs_;
    SimulatedNetwork network_;
    ReplicationServer server_;
    EntityTable<TestColumns> table_;
    std::mt19937_64 random_;
    std::vector<EntityId> mobs_;
    std::vector<Observer> observers_;
    std::vector<ITransport*> transports_;
    std::vector<std::unique_ptr<ClientReplica>> clients_;
    std::vector<PendingMove> pending_moves_;
    uint64_t tick_ = 0;
    bool converged_ = true;
    uint64_t checks_ = 0;
    uint64_t connects_ = 0;
    uint64_t disconnects_ = 0;
};

} // namespace network_test
