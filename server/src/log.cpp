// SPDX-License-Identifier: Apache-2.0
#include "tessera/server/log.hpp"

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

std::optional<LogLevel> ParseLogLevel(std::string_view name) {
    for (const LogLevel level : {LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error}) {
        if (name == LogLevelName(level)) {
            return level;
        }
    }
    return std::nullopt;
}

} // namespace tessera::server
