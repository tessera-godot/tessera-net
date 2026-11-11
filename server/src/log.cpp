// SPDX-License-Identifier: Apache-2.0
#include "tessera/server/log.hpp"

#include <cmath>
#include <cstdio>
#include <utility>

namespace tessera::server {

namespace {

// The calendar date of a day counted from 1970-01-01 (the proleptic Gregorian calendar), without the time
// zone database or any function that is not safe to call from several threads.
void DateOfDay(int64_t days, int& year, unsigned& month, unsigned& day) {
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned day_of_era = static_cast<unsigned>(days - era * 146097);
    const unsigned year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const unsigned day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const unsigned shifted_month = (5 * day_of_year + 2) / 153;
    day = day_of_year - (153 * shifted_month + 2) / 5 + 1;
    month = shifted_month < 10 ? shifted_month + 3 : shifted_month - 9;
    year = static_cast<int>(year_of_era) + static_cast<int>(era) * 400 + (month <= 2 ? 1 : 0);
}

const char* PaddedName(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO ";
    case LogLevel::Warn:
        return "WARN ";
    case LogLevel::Error:
        return "ERROR";
    }
    return "INFO ";
}

} // namespace

std::string_view LogLevelName(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warn:
        return "warn";
    case LogLevel::Error:
        return "error";
    }
    return "info";
}

std::optional<LogLevel> ParseLogLevel(std::string_view name) {
    for (const LogLevel level : {LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error}) {
        if (name == LogLevelName(level)) {
            return level;
        }
    }
    return std::nullopt;
}

std::string FormatLogLine(std::chrono::system_clock::time_point when, LogLevel level,
                          std::string_view message) {
    constexpr int64_t kMsPerDay = 24 * 60 * 60 * 1000;
    const int64_t ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()).count();
    int64_t days = ms / kMsPerDay;
    int64_t ms_of_day = ms % kMsPerDay;
    if (ms_of_day < 0) { // before 1970: round down, not toward zero
        ms_of_day += kMsPerDay;
        --days;
    }
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    DateOfDay(days, year, month, day);
    char stamp[96]; // room for the widest numbers, so that the compiler can see nothing is cut
    std::snprintf(stamp, sizeof(stamp), "%04d-%02u-%02uT%02d:%02d:%02d.%03dZ ", year, month, day,
                  static_cast<int>(ms_of_day / 3600000), static_cast<int>(ms_of_day / 60000 % 60),
                  static_cast<int>(ms_of_day / 1000 % 60), static_cast<int>(ms_of_day % 1000));
    std::string line(stamp);
    line += PaddedName(level);
    line += ' ';
    line += message;
    return line;
}

LogSink StdoutSink() {
    return [](const std::string& line) {
        std::fputs(line.c_str(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    };
}

Logger::Logger(LogLevel minimum, LogSink sink) : minimum_(minimum), sink_(std::move(sink)) {}

void Logger::Log(LogLevel level, std::string_view message) {
    if (!Enabled(level) || !sink_) {
        return;
    }
    const std::string line = FormatLogLine(std::chrono::system_clock::now(), level, message);
    std::lock_guard<std::mutex> lock(mutex_);
    sink_(line);
}

std::string FormatSummary(uint64_t tick, size_t players, uint64_t sent_bytes_per_s,
                          uint64_t received_bytes_per_s) {
    return "summary tick=" + std::to_string(tick) + " players=" + std::to_string(players) +
           " sent=" + std::to_string(sent_bytes_per_s) + " B/s recv=" + std::to_string(received_bytes_per_s) +
           " B/s";
}

SummaryReporter::SummaryReporter(uint32_t interval_seconds, Logger& log)
    : interval_(interval_seconds), log_(log) {}

void SummaryReporter::OnTick(Clock::time_point now, uint64_t tick, size_t players,
                             const net::TransportStats& stats) {
    if (interval_.count() == 0) {
        return;
    }
    if (!started_) {
        started_ = true;
        since_ = now;
        sent_before_ = stats.bytes_sent;
        received_before_ = stats.bytes_received;
        return;
    }
    const Clock::duration elapsed = now - since_;
    if (elapsed < interval_) {
        return;
    }
    const double seconds = std::chrono::duration<double>(elapsed).count();
    const auto per_second = [seconds](uint64_t now_bytes, uint64_t before) {
        const uint64_t difference = now_bytes >= before ? now_bytes - before : 0;
        return static_cast<uint64_t>(std::llround(static_cast<double>(difference) / seconds));
    };
    log_.Info(FormatSummary(tick, players, per_second(stats.bytes_sent, sent_before_),
                            per_second(stats.bytes_received, received_before_)));
    since_ = now;
    sent_before_ = stats.bytes_sent;
    received_before_ = stats.bytes_received;
}

} // namespace tessera::server
