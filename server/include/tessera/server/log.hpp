// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
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

} // namespace tessera::server
