// SPDX-License-Identifier: Apache-2.0
// The head-to-head bot: a replication client over ENet that does nothing but receive. It prints one JSON line
// per second with how many entities it sees and how late the state is.
//
//   tessera_h2h_bot port=9100 [host=127.0.0.1] [epoch_ms=E]
//
// Lines read from standard input: `checksum` prints the checksum of the state the bot holds, `quit` ends it.
#include "h2h_common.hpp"
#include "tessera/net/client_replica.hpp"
#include "tessera/net/transport/enet_transport.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace tessera::net;

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<bool> g_quit{false};
std::atomic<bool> g_want_checksum{false};

void ReadStandardInput() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.find("checksum") != std::string::npos) {
            g_want_checksum = true;
        } else if (line.find("quit") != std::string::npos) {
            break;
        }
    }
    g_quit = true; // also when the pipe closes: whoever started the bot is gone
}

std::string Format(const char* pattern, auto... values) {
    char text[512];
    std::snprintf(text, sizeof(text), pattern, values...);
    return text;
}

int Run(const h2h::Args& args) {
    const ReplicationSchema schema = h2h::MakeSchema();

    EnetClientConfig config;
    config.server_address = args.host;
    config.server_port = args.port;
    EnetClientTransport transport(config);
    ClientReplica client(transport, schema);

    std::thread(ReadStandardInput).detach();

    uint64_t last_tick_seen = 0;
    double lag_sum_ms = 0.0;
    uint32_t lag_samples = 0;
    auto window_start = Clock::now();
    auto next = Clock::now();

    while (!g_quit) {
        client.Poll();
        transport.Flush(); // the acknowledgement that Poll sent goes out now
        if (client.State() == ClientState::Disconnected || client.State() == ClientState::Rejected) {
            h2h::PrintJson("{\"event\":\"closed\"}");
            return 1;
        }

        const uint64_t latest = client.LatestTick();
        if (latest != last_tick_seen) {
            last_tick_seen = latest;
            if (args.epoch_ms != 0) {
                const double due_ms = static_cast<double>(args.epoch_ms) +
                                      static_cast<double>(latest) * (1000.0 / h2h::kTickHz);
                lag_sum_ms += static_cast<double>(h2h::SystemMs()) - due_ms;
                ++lag_samples;
            }
        }

        if (g_want_checksum.exchange(false)) {
            h2h::StateChecksum checksum;
            client.ForEachEntity([&](EntityId id, const uint32_t* words) {
                checksum.Add(id.raw(), words, schema.Find(id.type())->words);
            });
            h2h::PrintJson(
                Format("{\"event\":\"checksum\",\"entities\":%llu,\"checksum\":\"%s\",\"connection\":%u}",
                       static_cast<unsigned long long>(checksum.Count()), h2h::Hex(checksum.Value()).c_str(),
                       static_cast<unsigned>(client.ConnectionId())));
        }

        const auto now = Clock::now();
        if (now - window_start >= std::chrono::seconds(1)) {
            const double lag = lag_samples > 0 ? lag_sum_ms / lag_samples : 0.0;
            h2h::PrintJson(
                Format("{\"event\":\"client\",\"entities\":%zu,\"lag_ms\":%.1f,\"latest_tick\":%llu}",
                       client.VisibleCount(), lag, static_cast<unsigned long long>(latest)));
            lag_sum_ms = 0.0;
            lag_samples = 0;
            window_start = now;
        }

        next += std::chrono::milliseconds(10); // 100 Hz
        std::this_thread::sleep_until(next);
    }

    client.Disconnect();
    std::vector<TransportEvent> scratch;
    for (int i = 0; i < 10; ++i) { // let the goodbye reach the server
        transport.Poll(scratch);
        transport.Flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return Run(h2h::ParseArgs(argc, argv));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "tessera_h2h_bot: %s\n", error.what());
        return 1;
    }
}
