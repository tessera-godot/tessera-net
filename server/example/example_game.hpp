// SPDX-License-Identifier: Apache-2.0
// A small game for the example server: entities that walk around a square and bounce off its edges. Copy it
// as the starting point for your own game. What it shows:
//   - `DescribeWorld`: which properties are replicated, and who can see what;
//   - `OnStart`: making the tables and the first entities, with the shard id in every id;
//   - `OnTick`: moving the entities, telling the grid, and handing the changes to the replication layer;
//   - with `players` on, `OnPlayerConnected` and `OnPlayerDisconnected`: one entity for each player, and an
//     observer that stays with it, so that the player sees what is near;
//   - with `players` on, `OnPlayerInput`: reading what a client sent. The bytes come from the client, so
//     they are checked and cut to the range the game allows before they move anything.
#pragma once

#include "tessera/net/entity_table.hpp"
#include "tessera/net/replication/property_schema.hpp"
#include "tessera/net/spatial_grid.hpp"
#include "tessera/server/game_logic.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace tessera::server::example {

class ExampleGame final : public IGameLogic {
public:
    /// The entities live in a square of this side, in meters.
    static constexpr float kWorldSide = 200.0f;
    /// Meters per second along each ground axis.
    static constexpr float kMaxSpeed = 5.0f;
    /// Positions are sent in steps of a centimeter...
    static constexpr float kPositionStep = 0.01f;
    /// ...within this distance of the origin.
    static constexpr float kPositionRange = 2048.0f;

    /// Meters per second a player moves at full input.
    static constexpr float kPlayerSpeed = 8.0f;
    /// How far, in meters, a player sees other players.
    static constexpr float kPlayerRange = 100.0f;
    /// A player that has sent no input for this long stands still.
    static constexpr float kPlayerInputTimeoutSeconds = 0.5f;
    /// The entity type of the players.
    static constexpr uint32_t kPlayerType = 2;

    /// `walking` false keeps the entities where they start, which makes the state easy to check. `players`
    /// true makes one more entity for each connected player and moves it with the input the player sends.
    explicit ExampleGame(uint32_t entity_count = 100, bool walking = true, bool players = false)
        : entity_count_(entity_count), walking_(walking), players_(players) {
        schema_.AddType(kEntityType, {net::PropertyDescriptor::Position(
                                         TESSERA_COLUMN(Columns, x), TESSERA_COLUMN(Columns, y),
                                         TESSERA_COLUMN(Columns, z), kPositionStep, kPositionRange)});
        if (players_) {
            // The player's position, and which connection it belongs to (a connection id fits 16 bits).
            schema_.AddType(
                kPlayerType,
                {net::PropertyDescriptor::Position(
                     TESSERA_COLUMN(PlayerColumns, x), TESSERA_COLUMN(PlayerColumns, y),
                     TESSERA_COLUMN(PlayerColumns, z), kPositionStep, kPositionRange),
                 net::PropertyDescriptor::Integer(TESSERA_COLUMN(PlayerColumns, owner), false, 0, 65535)});
        }
    }

    WorldSetup DescribeWorld() override {
        WorldSetup setup;
        setup.schema = &schema_;
        // Every player sees every entity of the type, wherever it is.
        setup.relevancy.types[kEntityType] =
            net::TypeRelevancy{net::RelevancyMode::AlwaysRelevant, 0.0f, 0.0f};
        // The most entities one player can see at once (with players, room for 256 of them), rounded up to a
        // multiple of 1,024.
        const uint32_t visible = entity_count_ + (players_ ? 256u : 0u);
        setup.replication.max_visible_per_connection =
            std::clamp<uint32_t>((visible + 1023u) / 1024u * 1024u, 1024u, 262144u);
        if (players_) {
            // A player sees the other players within `kPlayerRange` of its own observer.
            setup.relevancy.types[kPlayerType] =
                net::TypeRelevancy{net::RelevancyMode::Spatial, kPlayerRange, 10.0f};
        }
        // No bandwidth budget: every change goes out. A real game sets one.
        setup.replication.rate_kbps = 0;
        setup.replication.max_buffer_bytes = 65535;
        return setup;
    }

    void OnStart(ServerContext& context) override {
        context_ = &context;
        // The shard id of the server goes into the id of every entity this table makes.
        table_ = std::make_unique<net::EntityTable<Columns>>(net::TableConfig{context.shard_id, kEntityType});
        ids_.reserve(entity_count_);
        where_.reserve(entity_count_);
        for (uint32_t i = 0; i < entity_count_; ++i) {
            const net::EntityId id = table_->Create();
            const auto found = table_->Find(id);
            Columns& columns = table_->Columns(found->page);
            const auto [x, z] = InitialPosition(i);
            columns.x[found->slot] = x;
            columns.y[found->slot] = 0.0f;
            columns.z[found->slot] = z;
            table_->MarkDirty(id);
            context.grid.Insert(id, x, 0.0f, z);
            context.replication.Relevancy().OnEntityCreated(id);
            ids_.push_back(id);
            where_.push_back(net::SlotLocation{found->page, found->slot});
            speed_x_.push_back((Unit(i, 2) * 2.0f - 1.0f) * kMaxSpeed);
            speed_z_.push_back((Unit(i, 3) * 2.0f - 1.0f) * kMaxSpeed);
        }
        if (players_) {
            player_table_ = std::make_unique<net::EntityTable<PlayerColumns>>(
                net::TableConfig{context.shard_id, kPlayerType});
            // An input that stops arriving stops the player after this many ticks.
            idle_limit_ticks_ = static_cast<uint32_t>(
                std::ceil(static_cast<double>(kPlayerInputTimeoutSeconds) * context.tick_rate_hz));
        }
        context.log.Info("example game: " + std::to_string(entity_count_) + " entities");
    }

    void OnTick(double dt_seconds) override {
        if (walking_) {
            Walk(static_cast<float>(dt_seconds));
        }
        if (players_) {
            MovePlayers(static_cast<float>(dt_seconds));
        }
        // Quantize what changed (and, the first time, everything), so that the network phase can send it.
        context_->replication.Refresh(*table_);
        if (player_table_) {
            context_->replication.Refresh(*player_table_);
        }
    }

    /// Makes the entity of the player and its observer.
    void OnPlayerConnected(net::ConnectionId player) override {
        if (!players_ || context_ == nullptr) {
            return;
        }
        const net::EntityId id = player_table_->Create();
        if (id.IsNull()) {
            context_->log.Warn("example game: no room for the entity of player " + std::to_string(player));
            return;
        }
        const auto found = player_table_->Find(id);
        const auto [x, z] = PlayerSpawn(player);
        PlayerColumns& columns = player_table_->Columns(found->page);
        columns.x[found->slot] = x;
        columns.y[found->slot] = 0.0f;
        columns.z[found->slot] = z;
        columns.owner[found->slot] = static_cast<uint16_t>(player);
        player_table_->MarkDirty(id);
        context_->grid.Insert(id, x, 0.0f, z);
        context_->replication.Relevancy().OnEntityCreated(id);
        players_by_id_.insert_or_assign(
            player, Player{id, net::SlotLocation{found->page, found->slot}, x, z, 0, 0, 0});
        Observe(player, x, z);
    }

    /// Two bytes: the axis along x and the axis along z, each from -127 to 127. Anything else a client sends
    /// is cut or ignored.
    void OnPlayerInput(net::ConnectionId player, uint32_t sequence, std::span<const uint8_t> bytes) override {
        (void)sequence;
        const auto found = players_by_id_.find(player);
        if (found == players_by_id_.end() || bytes.size() < 2) {
            return;
        }
        // -128 is not an axis value, so it is cut to -127.
        found->second.axis_x = std::max<int>(static_cast<int8_t>(bytes[0]), -127);
        found->second.axis_z = std::max<int>(static_cast<int8_t>(bytes[1]), -127);
        found->second.idle_ticks = 0;
    }

    /// Takes the entity of the player out of the world, in the order the engines need.
    void OnPlayerDisconnected(net::ConnectionId player) override {
        const auto found = players_by_id_.find(player);
        if (found == players_by_id_.end()) {
            return;
        }
        const net::EntityId id = found->second.entity;
        context_->grid.Remove(id);
        player_table_->Destroy(id);
        context_->replication.Relevancy().OnEntityDestroyed(id);
        players_by_id_.erase(found);
    }

    /// Where the entity of a player starts: near the middle of the world, a little to the side for each id.
    static std::pair<float, float> PlayerSpawn(net::ConnectionId player) {
        return {kWorldSide / 2 + 2.0f * static_cast<float>(player % 16), kWorldSide / 2};
    }

    void OnShutdown() override {
        if (context_ != nullptr) {
            context_->log.Info("example game stopped");
        }
    }

    /// Where entity number `index` starts: x and z, in meters, from 0 to `kWorldSide`. The same every run.
    static std::pair<float, float> InitialPosition(uint32_t index) {
        return {Unit(index, 0) * kWorldSide, Unit(index, 1) * kWorldSide};
    }

    uint32_t EntityCount() const noexcept {
        return entity_count_;
    }

    /// How many player entities exist now (0 without `players`).
    size_t PlayerEntityCount() const noexcept {
        return player_table_ ? player_table_->AliveCount() : 0;
    }

private:
    static constexpr uint32_t kEntityType = 1;

    struct Columns {
        float x[net::kPageSlots];
        float y[net::kPageSlots];
        float z[net::kPageSlots];
    };

    struct PlayerColumns {
        float x[net::kPageSlots];
        float y[net::kPageSlots];
        float z[net::kPageSlots];
        uint16_t owner[net::kPageSlots];
    };

    struct Player {
        net::EntityId entity;
        net::SlotLocation where;
        float x;
        float z;
        int axis_x; // -127 to 127
        int axis_z;
        uint32_t idle_ticks; // ticks since the last input
    };

    // A number from 0 to 1 that depends only on the entity and on which of its four numbers it is.
    static float Unit(uint32_t index, uint32_t which) {
        uint32_t h = (index * 4u + which + 1u) * 0x9E3779B9u; // 1 is the seed
        h ^= h >> 16;
        h *= 0x85EBCA6Bu;
        h ^= h >> 13;
        h *= 0xC2B2AE35u;
        h ^= h >> 16;
        return static_cast<float>(h >> 8) / 16777216.0f;
    }

    // The player's observer is where the player is, so that it sees what is near it.
    void Observe(net::ConnectionId player, float x, float z) {
        const net::Observer eye{x, 0.0f, z};
        context_->replication.Relevancy().SetObservers(player, std::span<const net::Observer>(&eye, 1));
    }

    void MovePlayers(float dt) {
        player_moves_.clear();
        moved_players_.clear();
        for (auto& [id, player] : players_by_id_) {
            if (++player.idle_ticks > idle_limit_ticks_) {
                player.axis_x = 0; // no input for too long: the player stops
                player.axis_z = 0;
            }
            float dx = static_cast<float>(player.axis_x) / 127.0f;
            float dz = static_cast<float>(player.axis_z) / 127.0f;
            const float length = std::sqrt(dx * dx + dz * dz);
            if (length == 0.0f) {
                continue;
            }
            if (length > 1.0f) {
                dx /= length; // a diagonal is not faster than a straight line
                dz /= length;
            }
            player.x = std::clamp(player.x + dx * kPlayerSpeed * dt, 0.0f, kWorldSide);
            player.z = std::clamp(player.z + dz * kPlayerSpeed * dt, 0.0f, kWorldSide);
            PlayerColumns& columns = player_table_->Columns(player.where.page);
            columns.x[player.where.slot] = player.x;
            columns.z[player.where.slot] = player.z;
            player_table_->MarkDirty(player.entity);
            player_moves_.push_back(net::PendingMove{player.entity, player.x, 0.0f, player.z});
            moved_players_.push_back(id);
        }
        context_->grid.ApplyMoves(player_moves_);
        for (const net::ConnectionId id : moved_players_) {
            const Player& player = players_by_id_.at(id);
            // Cheap: the engine only looks again after the observer moved 4 m.
            Observe(id, player.x, player.z);
        }
    }

    void Walk(float dt) {
        moves_.clear();
        for (size_t i = 0; i < ids_.size(); ++i) {
            Columns& columns = table_->Columns(where_[i].page);
            float x = columns.x[where_[i].slot] + speed_x_[i] * dt;
            float z = columns.z[where_[i].slot] + speed_z_[i] * dt;
            if (x < 0.0f || x > kWorldSide) {
                speed_x_[i] = -speed_x_[i];
                x = std::clamp(x, 0.0f, kWorldSide);
            }
            if (z < 0.0f || z > kWorldSide) {
                speed_z_[i] = -speed_z_[i];
                z = std::clamp(z, 0.0f, kWorldSide);
            }
            columns.x[where_[i].slot] = x;
            columns.z[where_[i].slot] = z;
            table_->MarkDirty(ids_[i]);
            moves_.push_back(net::PendingMove{ids_[i], x, 0.0f, z});
        }
        context_->grid.ApplyMoves(moves_);
    }

    uint32_t entity_count_;
    bool walking_;
    bool players_;
    net::ReplicationSchema schema_;
    ServerContext* context_ = nullptr;
    std::unique_ptr<net::EntityTable<Columns>> table_;
    std::vector<net::EntityId> ids_;
    std::vector<net::SlotLocation> where_;
    std::vector<float> speed_x_;
    std::vector<float> speed_z_;
    std::vector<net::PendingMove> moves_;
    // The players (only with `players`): their table, who they are and where, and scratch for one tick.
    std::unique_ptr<net::EntityTable<PlayerColumns>> player_table_;
    std::map<net::ConnectionId, Player> players_by_id_;
    uint32_t idle_limit_ticks_ = 0;
    std::vector<net::PendingMove> player_moves_;
    std::vector<net::ConnectionId> moved_players_;
};

} // namespace tessera::server::example
