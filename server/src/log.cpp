// SPDX-License-Identifier: Apache-2.0
#include "tessera/server/log.hpp"

#include <utility>

namespace tessera::server {

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

Logger::Logger(LogLevel minimum, LogSink sink) : minimum_(minimum), sink_(std::move(sink)) {}

void Logger::Log(LogLevel level, std::string_view message) {
    if (!Enabled(level) || !sink_) {
        return;
    }
    std::string line(LogLevelName(level));
    line += ' ';
    line += message;
    sink_(line);
}

std::optional<LogLevel> ParseLogLevel(std::string_view name) {
    for (const LogLevel level : {LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error}) {
        if (name == LogLevelName(level)) {
            return level;
        }
    }
    return std::nullopt;
}

} // namespace tessera::server
