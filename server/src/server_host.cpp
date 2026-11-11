// SPDX-License-Identifier: Apache-2.0
#include "tessera/server/server_host.hpp"

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace tessera::server {

namespace {

const ServerConfig& Checked(const ServerConfig& config) {
    Validate(config);
    return config;
}

WorldSetup Describe(IGameLogic& logic, const ServerConfig& config) {
    WorldSetup setup = logic.DescribeWorld();
    if (setup.schema == nullptr) {
        throw std::invalid_argument("IGameLogic::DescribeWorld returned no schema");
    }
    // The player limit and the tick rate are the server's settings, not the game's.
    setup.replication.max_connections = config.max_players;
    setup.replication.tick_rate = config.tick_rate_hz;
    return setup;
}

} // namespace

ServerHost::ServerHost(const ServerConfig& config, IGameLogic& logic, net::ITransport& transport, Logger& log,
                       std::function<void()> flush_transport)
    : config_(Checked(config)), logic_(logic), transport_(transport), log_(log),
      flush_transport_(std::move(flush_transport)), setup_(Describe(logic, config_)),
      jobs_(config_.worker_threads), grid_(setup_.grid),
      replication_(setup_.replication, setup_.relevancy, *setup_.schema, grid_, groups_, jobs_, transport_),
      loop_(net::TickConfig{static_cast<double>(config_.tick_rate_hz)}, jobs_),
      context_{config_.shard_id, config_.tick_rate_hz, replication_, grid_, groups_, jobs_, log_,
               [this] { RequestStop(); }},
      summary_(config_.summary_interval_s, log_) {
    loop_.SetPhaseHandler(net::Phase::Simulate,
                          [this](net::PhaseContext& tick) { logic_.OnTick(tick.dt_seconds); });
    loop_.SetPhaseHandler(net::Phase::Network, [this](net::PhaseContext& tick) {
        replication_.Update(tick.tick);
        if (flush_transport_) {
            flush_transport_();
        }
        summary_.OnTick(SummaryReporter::Clock::now(), tick.tick, replication_.ConnectionCount(),
                        transport_.Stats());
    });
}

ServerHost::~ServerHost() = default;

void ServerHost::Start() {
    if (started_) {
        return;
    }
    started_ = true;
    log_.Info("starting: port " + std::to_string(config_.port) + ", " + std::to_string(config_.max_players) +
              " players, " + std::to_string(config_.tick_rate_hz) + " ticks per second, shard " +
              std::to_string(config_.shard_id));
    replication_.SetCallbacks(
        [this](net::ConnectionId player) {
            log_.Info("player connected: " + std::to_string(player));
            logic_.OnPlayerConnected(player);
        },
        [this](net::ConnectionId player) {
            log_.Info("player disconnected: " + std::to_string(player));
            logic_.OnPlayerDisconnected(player);
        });
    logic_.OnStart(context_);
}

void ServerHost::Step() {
    if (shut_down_) {
        return;
    }
    Start();
    loop_.RunTick();
}

void ServerHost::Shutdown() {
    if (!started_ || shut_down_) {
        return;
    }
    shut_down_ = true;
    if (flush_transport_) {
        flush_transport_();
    }
    logic_.OnShutdown();
    log_.Info("stopped");
}

int ServerHost::Run() {
    if (shut_down_) {
        return 0;
    }
    int code = 0;
    try {
        Start();
        loop_.Run(stop_);
    } catch (const std::exception& error) {
        log_.Error(std::string("stopping because of an error: ") + error.what());
        code = 1;
    } catch (...) {
        log_.Error("stopping because of an error that is not an exception");
        code = 1;
    }
    Shutdown();
    return code;
}

void ServerHost::RequestStop() noexcept {
    stop_.store(true, std::memory_order_release);
}

bool ServerHost::StopRequested() const noexcept {
    return stop_.load(std::memory_order_acquire);
}

uint64_t ServerHost::TickCount() const noexcept {
    return loop_.Metrics().ticks;
}

net::ReplicationServer& ServerHost::Replication() noexcept {
    return replication_;
}

} // namespace tessera::server
