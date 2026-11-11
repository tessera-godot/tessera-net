// SPDX-License-Identifier: Apache-2.0
// The head-to-head server: a Tessera Net replication server over ENet, with N entities that move in a square
// world, in the scenario of the Godot baseline in bench/godot-replication. It prints one JSON line per second
// and, once the entities have stopped, the checksum of their replicated state.
//
//   tessera_h2h_server port=9100 entities=2000 clients=8 run=25 [moving=100] [threads=1]
//                      [relevancy=all|spatial] [range=250] [epoch_ms=0]
//
// Lines read from standard input: `quit` ends the program once the state has settled.
#include "h2h_common.hpp"
#include "tessera/net/entity_table.hpp"
#include "tessera/net/job_system.hpp"
#include "tessera/net/replication_server.hpp"
#include "tessera/net/spatial_grid.hpp"
#include "tessera/net/transport/enet_transport.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace tessera::net;

namespace {

using Clock = std::chrono::steady_clock;
using Millis = std::chrono::duration<double, std::milli>;

std::atomic<bool> g_quit{false};

void ReadStandardInput() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.find("quit") != std::string::npos) {
            break;
        }
    }
    g_quit = true; // also when the pipe closes: whoever started the server is gone
}

struct Location {
    uint32_t page;
    uint32_t slot;
};

// What one second of ticks adds up to.
struct Window {
    uint32_t ticks = 0;
    double process_ms = 0.0;
    double process_max_ms = 0.0;
    double physics_ms = 0.0;
    double update_ms = 0.0;
    double transport_ms = 0.0;
};

std::string Format(const char* pattern, auto... values) {
    char text[1024];
    std::snprintf(text, sizeof(text), pattern, values...);
    return text;
}

int Run(const h2h::Args& args) {
    const ReplicationSchema schema = h2h::MakeSchema();
    const bool spatial = args.relevancy == "spatial";
    const uint32_t entity_count = args.entities;
    const uint32_t tick_ms = 1000 / h2h::kTickHz;

    SpatialGrid grid(GridConfig{128.0f, 128.0f, 128.0f});
    RelevancyGroups groups;
    ThreadPoolJobSystem jobs(std::max(1u, args.threads));

    EnetServerConfig enet_config;
    enet_config.port = args.port;
    enet_config.max_clients = args.clients + 8;
    EnetServerTransport enet(enet_config);
    h2h::TimedTransport transport(enet);

    ServerConfig server_config;
    server_config.max_connections = args.clients + 8;
    server_config.max_visible_per_connection = std::max(1024u, (entity_count + 1023u) / 1024u * 1024u);
    server_config.tick_rate = h2h::kTickHz;
    server_config.rate_kbps = 0;            // no budget: every change is sent
    server_config.max_buffer_bytes = 65535; // a buffer of 10,000 positions must fit in one

    RelevancyConfig relevancy;
    relevancy.types[0] = spatial ? TypeRelevancy{RelevancyMode::Spatial, args.range, 8.0f}
                                 : TypeRelevancy{RelevancyMode::AlwaysRelevant, 0.0f, 0.0f};
    ReplicationServer server(server_config, relevancy, schema, grid, groups, jobs, transport);

    // The world: N entities with positions uniform in the square and speeds uniform in [-5, 5] m/s along x
    // and z (the baseline does not move them in height).
    EntityTable<h2h::Columns> table(TableConfig{0, 0});
    std::vector<EntityId> ids;
    std::vector<Location> where;
    std::vector<float> speed_x;
    std::vector<float> speed_z;
    ids.reserve(entity_count);
    where.reserve(entity_count);
    speed_x.reserve(entity_count);
    speed_z.reserve(entity_count);
    std::mt19937 rng(1);
    auto uniform = [&rng](float low, float high) {
        return std::uniform_real_distribution<float>(low, high)(rng);
    };
    for (uint32_t i = 0; i < entity_count; ++i) {
        const EntityId id = table.Create();
        const auto found = table.Find(id);
        h2h::Columns& columns = table.Columns(found->page);
        columns.x[found->slot] = uniform(0.0f, h2h::kWorldSide);
        columns.y[found->slot] = 0.0f;
        columns.z[found->slot] = uniform(0.0f, h2h::kWorldSide);
        table.MarkDirty(id);
        ids.push_back(id);
        where.push_back(Location{found->page, found->slot});
        speed_x.push_back(uniform(-h2h::kMaxSpeed, h2h::kMaxSpeed));
        speed_z.push_back(uniform(-h2h::kMaxSpeed, h2h::kMaxSpeed));
        if (spatial) {
            grid.Insert(id, columns.x[found->slot], 0.0f, columns.z[found->slot]);
        } else {
            server.Relevancy().OnEntityCreated(id);
        }
    }

    std::mt19937 observer_rng(2);
    std::vector<ConnectionId> connections;
    server.SetCallbacks(
        [&](ConnectionId id) {
            connections.push_back(id);
            if (spatial) { // each player sees from a fixed point
                std::uniform_real_distribution<float> where_in_world(0.0f, h2h::kWorldSide);
                const float x = where_in_world(observer_rng);
                const float z = where_in_world(observer_rng);
                const Observer eye{x, 0.0f, z};
                server.Relevancy().SetObservers(id, std::span<const Observer>(&eye, 1));
            }
        },
        [&](ConnectionId id) {
            connections.erase(std::remove(connections.begin(), connections.end(), id), connections.end());
        });

    std::thread(ReadStandardInput).detach();

    const uint32_t moving_count =
        static_cast<uint32_t>(static_cast<uint64_t>(entity_count) * std::min(args.moving, 100u) / 100u);
    const float step_seconds = 1.0f / static_cast<float>(h2h::kTickHz);
    std::vector<PendingMove> moves;
    bool moving = moving_count > 0;

    const auto program_start = Clock::now();
    const int64_t epoch_ms = args.epoch_ms != 0 ? args.epoch_ms : h2h::SystemMs();
    const auto epoch = Clock::now(); // tick n happens at epoch + n * 50 ms
    h2h::PrintJson(Format("{\"event\":\"spawned\",\"entities\":%u,\"port\":%u,\"epoch_ms\":%lld}",
                          entity_count, static_cast<unsigned>(enet.Port()),
                          static_cast<long long>(epoch_ms)));

    enum class Phase { Connecting, Running, Settling, Draining };
    Phase phase = Phase::Connecting;
    Clock::time_point all_connected_at{};
    Clock::time_point phase_deadline{};
    Window window;
    auto window_start = Clock::now();
    EnetWireStats last_wire{};
    uint64_t tick = 0;
    auto next = epoch;
    const TypeSchema* type_schema = schema.Find(0);

    for (;;) {
        ++tick;
        next += std::chrono::milliseconds(tick_ms); // by absolute deadline; a late tick is not skipped
        std::this_thread::sleep_until(next);

        const auto t0 = Clock::now();
        moves.clear();
        if (moving) {
            for (uint32_t i = 0; i < moving_count; ++i) {
                h2h::Columns& columns = table.Columns(where[i].page);
                float x = columns.x[where[i].slot] + speed_x[i] * step_seconds;
                float z = columns.z[where[i].slot] + speed_z[i] * step_seconds;
                if (x < 0.0f || x > h2h::kWorldSide) {
                    speed_x[i] = -speed_x[i];
                    x = std::clamp(x, 0.0f, h2h::kWorldSide);
                }
                if (z < 0.0f || z > h2h::kWorldSide) {
                    speed_z[i] = -speed_z[i];
                    z = std::clamp(z, 0.0f, h2h::kWorldSide);
                }
                columns.x[where[i].slot] = x;
                columns.z[where[i].slot] = z;
                table.MarkDirty(ids[i]);
                if (spatial) {
                    moves.push_back(PendingMove{ids[i], x, 0.0f, z});
                }
            }
            if (spatial) {
                grid.ApplyMoves(moves);
            }
        }
        const auto t1 = Clock::now();
        server.Refresh(table);
        server.Update(tick);
        const auto t2 = Clock::now();
        enet.Flush();
        const auto t3 = Clock::now();

        const double process = Millis(t3 - t0).count();
        window.ticks += 1;
        window.process_ms += process;
        window.process_max_ms = std::max(window.process_max_ms, process);
        window.physics_ms += Millis(t1 - t0).count();
        window.update_ms += Millis(t2 - t1).count();
        window.transport_ms += static_cast<double>(transport.TakePollNs() + transport.TakeSendNs()) / 1e6 +
                               Millis(t3 - t2).count();

        const auto now = Clock::now();
        if (now - window_start >= std::chrono::seconds(1)) {
            const double seconds = std::chrono::duration<double>(now - window_start).count();
            const EnetWireStats wire = enet.Wire();
            const double n = static_cast<double>(window.ticks);
            h2h::PrintJson(Format(
                "{\"event\":\"tick\",\"t\":%.1f,\"since_all\":%.1f,\"peers\":%zu,\"entities\":%u,\"fps\":%."
                "1f,"
                "\"process_ms\":%.3f,\"process_ms_max\":%.3f,\"physics_ms\":%.3f,\"update_ms\":%.3f,"
                "\"transport_ms\":%.3f,\"sent_bytes\":%llu,\"recv_bytes\":%llu}",
                std::chrono::duration<double>(now - program_start).count(),
                phase == Phase::Connecting ? -1.0
                                           : std::chrono::duration<double>(now - all_connected_at).count(),
                server.ConnectionCount(), entity_count, n / seconds, window.process_ms / n,
                window.process_max_ms, window.physics_ms / n, window.update_ms / n, window.transport_ms / n,
                static_cast<unsigned long long>(wire.bytes_sent - last_wire.bytes_sent),
                static_cast<unsigned long long>(wire.bytes_received - last_wire.bytes_received)));
            last_wire = wire;
            window = Window{};
            window_start = now;
        }

        switch (phase) {
        case Phase::Connecting:
            if (server.ConnectionCount() >= args.clients) {
                all_connected_at = now;
                phase = Phase::Running;
                h2h::PrintJson("{\"event\":\"all_connected\"}");
            } else if (now - program_start > std::chrono::seconds(120)) {
                h2h::PrintJson("{\"event\":\"error\",\"what\":\"clients did not connect\"}");
                return 2;
            }
            break;
        case Phase::Running:
            if (now - all_connected_at >= std::chrono::seconds(args.run)) {
                moving = false; // the entities stop; one more second lets everyone catch up
                phase = Phase::Settling;
                phase_deadline = now + std::chrono::seconds(1);
            }
            break;
        case Phase::Settling:
            if (now >= phase_deadline) {
                h2h::StateChecksum checksum;
                for (const EntityId id : ids) {
                    if (const uint32_t* words = server.Replication().Store().Find(id)) {
                        checksum.Add(id.raw(), words, type_schema->words);
                    }
                }
                std::string visible;
                if (spatial) {
                    for (const ConnectionId id : connections) {
                        if (!visible.empty()) {
                            visible += ",";
                        }
                        visible += Format("{\"connection\":%u,\"count\":%u}", static_cast<unsigned>(id),
                                          static_cast<unsigned>(server.Relevancy().VisibleCount(id)));
                    }
                }
                h2h::PrintJson(
                    Format("{\"event\":\"settled\",\"entities\":%llu,\"checksum\":\"%s\",\"peers\":%zu,"
                           "\"visible\":[%s]}",
                           static_cast<unsigned long long>(checksum.Count()),
                           h2h::Hex(checksum.Value()).c_str(), server.ConnectionCount(), visible.c_str()));
                phase = Phase::Draining;
                phase_deadline = now + std::chrono::seconds(20);
            }
            break;
        case Phase::Draining:
            if (g_quit || now >= phase_deadline) {
                h2h::PrintJson("{\"event\":\"done\"}");
                return 0;
            }
            break;
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        return Run(h2h::ParseArgs(argc, argv));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "tessera_h2h_server: %s\n", error.what());
        return 1;
    }
}
