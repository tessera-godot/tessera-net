// SPDX-License-Identifier: Apache-2.0
// A small game for the example server: entities that walk around a square and bounce off its edges. Copy it
// as the starting point for your own game. What it shows:
//   - `DescribeWorld`: which properties are replicated, and who can see what;
//   - `OnStart`: making the tables and the first entities, with the shard id in every id;
//   - `OnTick`: moving the entities, telling the grid, and handing the changes to the replication layer.
#pragma once

#include "tessera/net/entity_table.hpp"
#include "tessera/net/replication/property_schema.hpp"
#include "tessera/net/spatial_grid.hpp"
#include "tessera/server/game_logic.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
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

    /// `walking` false keeps the entities where they start, which makes the state easy to check.
    explicit ExampleGame(uint32_t entity_count = 100, bool walking = true)
        : entity_count_(entity_count), walking_(walking) {
        schema_.AddType(kEntityType, {net::PropertyDescriptor::Position(
                                         TESSERA_COLUMN(Columns, x), TESSERA_COLUMN(Columns, y),
                                         TESSERA_COLUMN(Columns, z), kPositionStep, kPositionRange)});
    }

    WorldSetup DescribeWorld() override {
        WorldSetup setup;
        setup.schema = &schema_;
        // Every player sees every entity of the type, wherever it is.
        setup.relevancy.types[kEntityType] =
            net::TypeRelevancy{net::RelevancyMode::AlwaysRelevant, 0.0f, 0.0f};
        // The most entities one player can see at once, rounded up to a multiple of 1,024.
        setup.replication.max_visible_per_connection =
            std::clamp<uint32_t>((entity_count_ + 1023u) / 1024u * 1024u, 1024u, 262144u);
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
        context.log.Info("example game: " + std::to_string(entity_count_) + " entities");
    }

    void OnTick(double dt_seconds) override {
        if (walking_) {
            Walk(static_cast<float>(dt_seconds));
        }
        // Quantize what changed (and, the first time, everything), so that the network phase can send it.
        context_->replication.Refresh(*table_);
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

private:
    static constexpr uint32_t kEntityType = 1;

    struct Columns {
        float x[net::kPageSlots];
        float y[net::kPageSlots];
        float z[net::kPageSlots];
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
    net::ReplicationSchema schema_;
    ServerContext* context_ = nullptr;
    std::unique_ptr<net::EntityTable<Columns>> table_;
    std::vector<net::EntityId> ids_;
    std::vector<net::SlotLocation> where_;
    std::vector<float> speed_x_;
    std::vector<float> speed_z_;
    std::vector<net::PendingMove> moves_;
};

} // namespace tessera::server::example
