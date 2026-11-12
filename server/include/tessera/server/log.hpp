// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tessera/net/transport/transport.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace tessera::server {

enum class LogLevel : uint8_t {
    Debug,
    Info,
    Warn,
    Error,
};

/// "debug", "info", "warn" or "error".
std::string_view LogLevelName(LogLevel level) noexcept;

/// The level with that exact (lowercase) name, or nothing.
std::optional<LogLevel> ParseLogLevel(std::string_view name);

/// One line of the log: the time in UTC with milliseconds, the level in capitals padded to five columns, and
/// the message, as in `2026-11-11T18:47:03.042Z INFO  listening on port 7777`.
std::string FormatLogLine(std::chrono::system_clock::time_point when, LogLevel level,
                          std::string_view message);

/// Receives each line that is written, without the line break.
using LogSink = std::function<void(const std::string& line)>;

/// A sink that writes to standard output, one line at a time, and flushes after each so that whoever reads
/// the process sees a line as soon as it is written.
LogSink StdoutSink();

/// Writes the lines at or above a minimum level to a sink, with the time and the level in front. Safe to use
/// from several threads (the sink is called under a lock), but not from a signal handler.
class Logger {
public:
    Logger(LogLevel minimum, LogSink sink);

    bool Enabled(LogLevel level) const noexcept {
        return level >= minimum_;
    }
    void Log(LogLevel level, std::string_view message);
    void Debug(std::string_view message) {
        Log(LogLevel::Debug, message);
    }
    void Info(std::string_view message) {
        Log(LogLevel::Info, message);
    }
    void Warn(std::string_view message) {
        Log(LogLevel::Warn, message);
    }
    void Error(std::string_view message) {
        Log(LogLevel::Error, message);
    }

private:
    LogLevel minimum_;
    LogSink sink_;
    std::mutex mutex_;
};

/// `summary tick=<n> players=<n> sent=<n> B/s recv=<n> B/s`
std::string FormatSummary(uint64_t tick, size_t players, uint64_t sent_bytes_per_s,
                          uint64_t received_bytes_per_s);

/// Writes a summary line at most once per interval. Call `OnTick` every tick; the bytes per second are the
/// difference of the transport's counters over the time that really went by.
class SummaryReporter {
public:
    using Clock = std::chrono::steady_clock;

    /// An interval of 0 seconds never writes anything.
    SummaryReporter(uint32_t interval_seconds, Logger& log);

    /// The first call only marks where the counting starts. After a long pause one line is written, not one
    /// for each interval that was missed.
    void OnTick(Clock::time_point now, uint64_t tick, size_t players, const net::TransportStats& stats);

private:
    std::chrono::seconds interval_;
    Logger& log_;
    bool started_ = false;
    Clock::time_point since_{};
    uint64_t sent_before_ = 0;
    uint64_t received_before_ = 0;
};

} // namespace tessera::server
