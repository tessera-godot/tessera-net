// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
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

/// Receives each line that is written, without the line break.
using LogSink = std::function<void(const std::string& line)>;

/// Writes lines at or above a minimum level to a sink.
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
};

} // namespace tessera::server
