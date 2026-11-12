// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tessera/server/log.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace tessera::server {

/// What a server needs to know to start. Every field has a default that works, so a game only sets what it
/// cares about. (`tessera::net::ServerConfig` is the replication layer's own, narrower settings.)
///
/// In a file or on the command line, each field has a text key (in brackets) and takes a plain decimal
/// number, except `log_level`.
struct ServerConfig {
    /// [port] UDP port to listen on, 1 to 65,535.
    uint16_t port = 7777;
    /// [max_players] Players at once, 1 to 65,535.
    uint32_t max_players = 128;
    /// [tick_rate] Ticks per second, 1 to 1,000.
    uint32_t tick_rate_hz = 30;
    /// [threads] Threads for parallel work, the main one included, 1 to 256.
    uint32_t worker_threads = 1;
    /// [shard_id] Written into every entity id, 0 to 4,095.
    uint32_t shard_id = 0;
    /// [log_level] debug, info, warn or error.
    LogLevel log_level = LogLevel::Info;
    /// [summary_interval] Seconds between summary lines, 0 to 3,600; 0 turns them off.
    uint32_t summary_interval_s = 0;
};

/// A setting that cannot be used. `what()` is "<key>: <message>"; `Key()` is the key alone. For a line of a
/// file that has no key it is "line N", and for a file that cannot be read it is "config".
class ConfigError final : public std::runtime_error {
public:
    ConfigError(std::string key, const std::string& message);
    const std::string& Key() const noexcept {
        return key_;
    }

private:
    std::string key_;
};

/// Throws `ConfigError`, naming the key, for the first field that is out of range.
void Validate(const ServerConfig& config);

/// Sets one field from its text key and value. Throws `ConfigError` for a key that is not known, a value that
/// is not a plain decimal number (or a log level) and a number out of range.
void ApplySetting(ServerConfig& config, std::string_view key, std::string_view value);

/// Reads `key=value` lines on top of `base`. A line starting with `#` is a comment; blank lines, spaces
/// around the key and the value, `\r\n` line ends and a UTF-8 byte order mark at the start are accepted.
/// Throws `ConfigError` for a key that appears twice, an empty key or value, a line without `=` and any bad
/// setting.
ServerConfig ParseConfigText(std::string_view text, ServerConfig base = {});

/// `ParseConfigText` on a file. Throws `ConfigError("config", ...)` naming the path if it cannot be read.
ServerConfig LoadConfigFile(const std::filesystem::path& path, ServerConfig base = {});

/// Reads arguments of the form `--key=value` on top of `base`. Throws `ConfigError` for an argument of
/// another form, a key that appears twice and any bad setting. `--config` is not a setting; see
/// `ResolveConfig`.
ServerConfig ParseArguments(std::span<const std::string_view> args, ServerConfig base = {});

/// The usual way to read a configuration: the defaults, then the file named by `--config=<path>` (if any),
/// then the other arguments on top, then `Validate`.
ServerConfig ResolveConfig(std::span<const std::string_view> args);

/// Usage text: the keys, their defaults and their ranges.
std::string ConfigHelp();

} // namespace tessera::server
