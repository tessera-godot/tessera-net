// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tessera/net/protocol.hpp"
#include "tessera/net/relevancy/relevancy_config.hpp"
#include "tessera/net/replication/property_schema.hpp"
#include "tessera/net/replication_server.hpp"
#include "tessera/net/spatial_grid.hpp"
#include "tessera/server/log.hpp"

#include <cstdint>
#include <functional>

namespace tessera::server {

/// What the server needs to know about a game's world before it can build the replication layer: the
/// replicated schema, how relevancy works and how the grid is cut. The host overrides
/// `replication.max_connections` with the configured number of players and `replication.tick_rate` with the
/// configured tick rate; everything else in `replication` (the bandwidth budget, for one) is the game's to
/// set.
struct WorldSetup {
    const net::ReplicationSchema* schema = nullptr; ///< Required; must outlive the host.
    net::RelevancyConfig relevancy;
    net::GridConfig grid{128.0f, 128.0f, 128.0f};
    net::ServerConfig replication = [] {
        net::ServerConfig config;
        config.max_visible_per_connection = 4096;
        return config;
    }();
};

/// What the host hands the game when it starts. The references stay valid until `OnShutdown` has returned.
struct ServerContext {
    uint32_t shard_id;     ///< Put it in `TableConfig{shard, type}` so that it reaches the entity ids.
    uint32_t tick_rate_hz;
    net::ReplicationServer& replication;
    net::SpatialGrid& grid;
    net::RelevancyGroups& groups;
    net::IJobSystem& jobs;
    Logger& log;
    std::function<void()> request_stop; ///< Asks the host to stop; safe from any thread and from a callback.
};

/// What a game implements to run on the server. The first version of this interface; it may change.
///
/// All of it runs on the thread that runs the host. None of the callbacks may throw: an exception that leaves
/// `OnTick` ends the run (the host logs it, calls `OnShutdown` and `Run` returns 1).
class IGameLogic {
public:
    virtual ~IGameLogic() = default;

    /// The world the host builds the replication layer for. Called once, from the host's constructor.
    virtual WorldSetup DescribeWorld() = 0;

    /// Called once, before the first tick. Create the tables and the first entities here.
    virtual void OnStart(ServerContext& context) {
        (void)context;
    }

    /// One tick of simulation, every `1 / tick_rate_hz` seconds. Move the entities, mark what changed, and
    /// call
    /// `context.replication.Refresh(table)` for each table at the end; the host then runs the network phase.
    virtual void OnTick(double dt_seconds) = 0;

    /// A player was accepted / has gone. Called while the network phase reads the transport.
    virtual void OnPlayerConnected(net::ConnectionId player) {
        (void)player;
    }
    virtual void OnPlayerDisconnected(net::ConnectionId player) {
        (void)player;
    }

    /// Called once, after the last tick and after what was still pending has been handed to the transport.
    virtual void OnShutdown() {}
};

} // namespace tessera::server
