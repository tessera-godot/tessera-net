// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/client_replica.hpp"
#include "tessera/net/entity_table.hpp"
#include "tessera/net/transport/simulated_network.hpp"
#include "tessera/server/game_logic.hpp"
#include "tessera/server/log.hpp"
#include "tessera/server/server_host.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace tessera::net;
using namespace tessera::server;

namespace {

using Clock = SummaryReporter::Clock;
using std::chrono::milliseconds;
using std::chrono::seconds;

// A game with nothing in it: the host needs a schema, and that is all.
class EmptyGame final : public IGameLogic {
public:
    WorldSetup DescribeWorld() override {
        static const ReplicationSchema schema = [] {
            struct Columns {
                float x[kPageSlots];
                float y[kPageSlots];
                float z[kPageSlots];
            };
            ReplicationSchema built;
            built.AddType(1, {PropertyDescriptor::Position(TESSERA_COLUMN(Columns, x),
                                                           TESSERA_COLUMN(Columns, y),
                                                           TESSERA_COLUMN(Columns, z), 0.01f, 5000.0f)});
            return built;
        }();
        WorldSetup setup;
        setup.schema = &schema;
        return setup;
    }
    void OnTick(double) override {}
};

// Collects what a logger writes.
struct Lines {
    LogSink Sink() {
        return [this](const std::string& line) {
            std::lock_guard<std::mutex> lock(mutex);
            lines.push_back(line);
        };
    }
    bool Has(const std::string& text) const {
        for (const std::string& line : lines) {
            if (line.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
    std::mutex mutex;
    std::vector<std::string> lines;
};

TransportStats Stats(uint64_t sent, uint64_t received) {
    TransportStats stats;
    stats.bytes_sent = sent;
    stats.bytes_received = received;
    return stats;
}

} // namespace

TESSERA_TEST(log_line_has_utc_timestamp_with_milliseconds_level_and_message) {
    // 2026-11-11 18:47:03.042 UTC
    const auto when = std::chrono::system_clock::time_point(seconds(1794422823) + milliseconds(42));
    TESSERA_CHECK(FormatLogLine(when, LogLevel::Info, "listening on port 7777") ==
                  "2026-11-11T18:47:03.042Z INFO  listening on port 7777");
    TESSERA_CHECK(FormatLogLine(when, LogLevel::Debug, "x") == "2026-11-11T18:47:03.042Z DEBUG x");
    TESSERA_CHECK(FormatLogLine(when, LogLevel::Warn, "x") == "2026-11-11T18:47:03.042Z WARN  x");
    TESSERA_CHECK(FormatLogLine(when, LogLevel::Error, "x") == "2026-11-11T18:47:03.042Z ERROR x");
    // A leap day, the last second of it.
    const auto leap = std::chrono::system_clock::time_point(seconds(1709251199) + milliseconds(999));
    TESSERA_CHECK(FormatLogLine(leap, LogLevel::Info, "y") == "2024-02-29T23:59:59.999Z INFO  y");
    // The epoch itself.
    TESSERA_CHECK(FormatLogLine(std::chrono::system_clock::time_point{}, LogLevel::Info, "z") ==
                  "1970-01-01T00:00:00.000Z INFO  z");
}

TESSERA_TEST(log_level_names_round_trip) {
    for (const LogLevel level : {LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error}) {
        const auto parsed = ParseLogLevel(LogLevelName(level));
        TESSERA_CHECK(parsed.has_value() && *parsed == level);
    }
    TESSERA_CHECK(LogLevelName(LogLevel::Warn) == "warn");
    TESSERA_CHECK(!ParseLogLevel("").has_value());
    TESSERA_CHECK(!ParseLogLevel("warning").has_value());
    TESSERA_CHECK(!ParseLogLevel("INFO").has_value());
}

TESSERA_TEST(logger_drops_lines_below_the_minimum) {
    Lines lines;
    Logger log(LogLevel::Warn, lines.Sink());
    log.Debug("d");
    log.Info("i");
    log.Warn("w");
    log.Error("e");
    TESSERA_CHECK(lines.lines.size() == 2);
    TESSERA_CHECK(!log.Enabled(LogLevel::Info));
    TESSERA_CHECK(log.Enabled(LogLevel::Warn) && log.Enabled(LogLevel::Error));
    TESSERA_CHECK(lines.Has(" w") && lines.Has(" e") && !lines.Has(" i") && !lines.Has(" d"));
}

TESSERA_TEST(logger_keeps_lines_whole_when_two_threads_write) {
    // The sink is not safe by itself (it appends to a string with no lock of its own): the logger has to
    // hand it one line at a time.
    std::string all;
    Logger log(LogLevel::Info, [&all](const std::string& line) {
        all += line;
        all += '\n';
    });
    constexpr int kLines = 500;
    auto writer = [&log](const char* marker) {
        for (int i = 0; i < kLines; ++i) {
            log.Info(std::string("a line from a thread that goes on for a while, to make it long ") + marker);
        }
    };
    std::thread first(writer, "ONE");
    std::thread second(writer, "TWO");
    first.join();
    second.join();
    size_t count = 0;
    size_t start = 0;
    bool whole = true;
    while (start < all.size()) {
        const size_t end = all.find('\n', start);
        if (end == std::string::npos) {
            whole = false;
            break;
        }
        const std::string line = all.substr(start, end - start);
        const bool marked = line.size() >= 3 && (line.compare(line.size() - 3, 3, "ONE") == 0 ||
                                                 line.compare(line.size() - 3, 3, "TWO") == 0);
        whole = whole && marked;
        ++count;
        start = end + 1;
    }
    TESSERA_CHECK(whole);
    TESSERA_CHECK(count == 2 * kLines);
}

TESSERA_TEST(summary_is_silent_when_the_interval_is_0) {
    Lines lines;
    Logger log(LogLevel::Debug, lines.Sink());
    SummaryReporter reporter(0, log);
    const Clock::time_point start = Clock::now();
    for (int i = 0; i < 10; ++i) {
        reporter.OnTick(start + seconds(i), static_cast<uint64_t>(i), 3, Stats(1000u * i, 100u * i));
    }
    TESSERA_CHECK(lines.lines.empty());
}

TESSERA_TEST(summary_writes_once_per_interval) {
    Lines lines;
    Logger log(LogLevel::Debug, lines.Sink());
    SummaryReporter reporter(1, log);
    const Clock::time_point start = Clock::now();
    const int half_seconds[] = {0, 500, 1000, 1500, 2000};
    uint64_t tick = 0;
    for (const int ms : half_seconds) {
        ++tick;
        reporter.OnTick(start + milliseconds(ms), tick, 2, Stats(100u * tick, 10u * tick));
    }
    // The first call only marks the start; the lines come at 1.0 s and 2.0 s.
    TESSERA_CHECK(lines.lines.size() == 2);
}

TESSERA_TEST(summary_reports_bytes_per_second_from_the_difference) {
    Lines lines;
    Logger log(LogLevel::Debug, lines.Sink());
    SummaryReporter reporter(1, log);
    const Clock::time_point start = Clock::now();
    // Whatever had been sent before the reporter started does not count.
    reporter.OnTick(start, 0, 1, Stats(50000, 9000));
    reporter.OnTick(start + milliseconds(1500), 45, 1, Stats(51500, 9300));
    TESSERA_CHECK(lines.lines.size() == 1);
    TESSERA_CHECK(lines.Has("sent=1000 B/s"));
    TESSERA_CHECK(lines.Has("recv=200 B/s"));
    TESSERA_CHECK(lines.Has("tick=45"));
    TESSERA_CHECK(lines.Has("players=1"));
}

TESSERA_TEST(summary_after_a_long_stall_writes_one_line) {
    Lines lines;
    Logger log(LogLevel::Debug, lines.Sink());
    SummaryReporter reporter(1, log);
    const Clock::time_point start = Clock::now();
    reporter.OnTick(start, 0, 0, Stats(0, 0));
    reporter.OnTick(start + seconds(10), 300, 0, Stats(10000, 0)); // ten intervals went by without a call
    TESSERA_CHECK(lines.lines.size() == 1);
    TESSERA_CHECK(lines.Has("sent=1000 B/s")); // spread over the ten real seconds
    reporter.OnTick(start + seconds(10) + milliseconds(500), 315, 0, Stats(10500, 0));
    TESSERA_CHECK(lines.lines.size() == 1); // and no catching up: the next line is a second later
    reporter.OnTick(start + seconds(11), 330, 0, Stats(11000, 0));
    TESSERA_CHECK(lines.lines.size() == 2);
}

TESSERA_TEST(summary_text_format) {
    TESSERA_CHECK(FormatSummary(120, 3, 4500, 800) ==
                  "summary tick=120 players=3 sent=4500 B/s recv=800 B/s");
    TESSERA_CHECK(FormatSummary(0, 0, 0, 0) == "summary tick=0 players=0 sent=0 B/s recv=0 B/s");
}

TESSERA_TEST(host_logs_start_players_and_stop_at_info_and_hides_debug) {
    Lines lines;
    Logger log(LogLevel::Info, lines.Sink());
    log.Debug("never shown");
    EmptyGame game;
    SimulatedNetwork network(NetworkConditions{});
    tessera::server::ServerConfig config;
    config.max_players = 8;
    config.tick_rate_hz = 100;
    ServerHost host(config, game, network.Server(), log);
    const ReplicationSchema& schema = *game.DescribeWorld().schema;
    ClientReplica client(*network.AddClient(), schema);
    uint64_t now = 0;
    for (int i = 0; i < 6; ++i) {
        network.Advance(++now);
        client.Poll();
        host.Step();
    }
    client.Disconnect();
    for (int i = 0; i < 3; ++i) {
        network.Advance(++now);
        host.Step();
    }
    host.Shutdown();
    TESSERA_CHECK(lines.Has("starting"));
    TESSERA_CHECK(lines.Has("player connected"));
    TESSERA_CHECK(lines.Has("player disconnected"));
    TESSERA_CHECK(lines.Has("stopped"));
    TESSERA_CHECK(!lines.Has("never shown"));
    // Every line starts with a UTC time stamp.
    for (const std::string& line : lines.lines) {
        TESSERA_CHECK(line.size() > 24 && line[4] == '-' && line[10] == 'T' && line[23] == 'Z');
    }
}

TESSERA_TEST(host_writes_a_summary_when_enabled) {
    Lines lines;
    Logger log(LogLevel::Info, lines.Sink());
    EmptyGame game;
    SimulatedNetwork network(NetworkConditions{});
    tessera::server::ServerConfig config;
    config.max_players = 8;
    config.tick_rate_hz = 50;
    config.summary_interval_s = 1;
    ServerHost host(config, game, network.Server(), log);
    std::thread runner([&host] { host.Run(); });
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < seconds(5)) {
        {
            std::lock_guard<std::mutex> lock(lines.mutex);
            if (lines.Has("summary tick=")) {
                break;
            }
        }
        std::this_thread::sleep_for(milliseconds(20));
    }
    host.RequestStop();
    runner.join();
    TESSERA_CHECK(lines.Has("summary tick="));
    TESSERA_CHECK(lines.Has("players=0"));
}
