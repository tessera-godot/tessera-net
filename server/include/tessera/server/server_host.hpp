// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tessera/net/job_system.hpp"
#include "tessera/net/relevancy/relevancy_groups.hpp"
#include "tessera/net/replication_server.hpp"
#include "tessera/net/spatial_grid.hpp"
#include "tessera/net/tick_loop.hpp"
#include "tessera/net/transport/transport.hpp"
#include "tessera/server/game_logic.hpp"
#include "tessera/server/log.hpp"
#include "tessera/server/server_config.hpp"

#include <atomic>
#include <cstdint>
#include <functional>

namespace tessera::server {

/// Runs a game on a server: it builds the world the game describes, runs the tick loop at the configured rate
/// and stops cleanly when asked. It owns the job system, the grid, the relevancy groups, the replication
/// server and the tick loop; the transport, the game and the logger belong to the caller and must outlive it.
/// There is no global state, so several hosts can live in one process.
///
/// Each tick has two phases: the game's `OnTick`, then the network phase (`ReplicationServer::Update`, then
/// `flush_transport`).
class ServerHost {
public:
    /// Throws `ConfigError` for a bad `config`, and `std::invalid_argument` if the game describes no schema
    /// (or one the replication layer cannot use). `flush_transport`, if given, hands what the transport has
    /// queued to the network; it runs at the end of every network phase and once more before `OnShutdown`.
    ServerHost(const ServerConfig& config, IGameLogic& logic, net::ITransport& transport, Logger& log,
               std::function<void()> flush_transport = {});
    ~ServerHost();
    ServerHost(const ServerHost&) = delete;
    ServerHost& operator=(const ServerHost&) = delete;
    ServerHost(ServerHost&&) = delete;
    ServerHost& operator=(ServerHost&&) = delete;

    /// Calls `OnStart`. Happens once; `Step` and `Run` call it when needed. If `OnStart` throws, the exception
    /// reaches the caller and the host stays failed: later `Step` calls do nothing and `Run` returns 1.
    void Start();

    /// Runs one tick now, without waiting. For tests, and for a game that runs its own loop.
    void Step();

    /// Hands what is pending to the transport and calls `OnShutdown`. Does nothing before `Start`, and after
    /// the first call. From then on `Step` and `Run` do nothing: nobody reads the transport any more, so no
    /// one new is accepted (the owner closes the transport). If the flush or `OnShutdown` throws, the other
    /// one still runs and the first exception is thrown at the end; `Run` catches it.
    void Shutdown();

    /// Starts, ticks in real time until a stop is requested, and shuts down. Returns 0 after a stop, and 1 if
    /// something thrown by the game or by the final flush ended the run (it is logged). Nothing escapes.
    int Run();

    /// Asks `Run` to stop after the tick in progress. Safe from another thread, from a signal handler and
    /// from inside a callback; asking again changes nothing.
    void RequestStop() noexcept;
    bool StopRequested() const noexcept;

    /// Ticks run so far.
    uint64_t TickCount() const noexcept;

    net::ReplicationServer& Replication() noexcept;

private:
    ServerConfig config_;
    IGameLogic& logic_;
    net::ITransport& transport_;
    Logger& log_;
    std::function<void()> flush_transport_;
    WorldSetup setup_;
    net::ThreadPoolJobSystem jobs_;
    net::SpatialGrid grid_;
    net::RelevancyGroups groups_;
    net::ReplicationServer replication_;
    net::TickLoop loop_;
    ServerContext context_;
    SummaryReporter summary_;
    std::atomic<bool> stop_{false};
    bool started_ = false;
    bool start_failed_ = false;
    bool shut_down_ = false;
};

} // namespace tessera::server
