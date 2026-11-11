// SPDX-License-Identifier: Apache-2.0
#include "tessera/server/server_config.hpp"

#include "tessera/net/entity_id.hpp"

#include <charconv>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <vector>

namespace tessera::server {

namespace {

struct Range {
    uint64_t low;
    uint64_t high;
};

constexpr Range kPortRange{1, 65535};
constexpr Range kPlayersRange{1, 65535};
constexpr Range kTickRateRange{1, 1000};
constexpr Range kThreadsRange{1, 256};
constexpr Range kShardRange{0, tessera::net::EntityId::kMaxShard};
constexpr Range kSummaryRange{0, 3600};

std::string RangeText(const Range& range) {
    return "must be a whole number from " + std::to_string(range.low) + " to " + std::to_string(range.high);
}

void CheckRange(const char* key, uint64_t value, const Range& range) {
    if (value < range.low || value > range.high) {
        throw ConfigError(key, RangeText(range) + ", not " + std::to_string(value));
    }
}

// Only decimal digits: no sign, no spaces, no exponent, nothing after the number.
uint64_t ParseNumber(const char* key, std::string_view value, const Range& range) {
    if (value.empty()) {
        throw ConfigError(key, "missing value");
    }
    for (const char c : value) {
        if (c < '0' || c > '9') {
            throw ConfigError(key, RangeText(range) + ", not \"" + std::string(value) + "\"");
        }
    }
    uint64_t number = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        throw ConfigError(key, RangeText(range) + ", not \"" + std::string(value) + "\"");
    }
    CheckRange(key, number, range);
    return number;
}

std::string_view Trim(std::string_view text) {
    constexpr std::string_view kSpace = " \t\r\n";
    const size_t first = text.find_first_not_of(kSpace);
    if (first == std::string_view::npos) {
        return {};
    }
    const size_t last = text.find_last_not_of(kSpace);
    return text.substr(first, last - first + 1);
}

} // namespace

ConfigError::ConfigError(std::string key, const std::string& message)
    : std::runtime_error(key + ": " + message), key_(std::move(key)) {}

void Validate(const ServerConfig& config) {
    CheckRange("port", config.port, kPortRange);
    CheckRange("max_players", config.max_players, kPlayersRange);
    CheckRange("tick_rate", config.tick_rate_hz, kTickRateRange);
    CheckRange("threads", config.worker_threads, kThreadsRange);
    CheckRange("shard_id", config.shard_id, kShardRange);
    if (config.log_level > LogLevel::Error) {
        throw ConfigError("log_level", "must be debug, info, warn or error");
    }
    CheckRange("summary_interval", config.summary_interval_s, kSummaryRange);
}

void ApplySetting(ServerConfig& config, std::string_view key, std::string_view value) {
    if (key == "port") {
        config.port = static_cast<uint16_t>(ParseNumber("port", value, kPortRange));
    } else if (key == "max_players") {
        config.max_players = static_cast<uint32_t>(ParseNumber("max_players", value, kPlayersRange));
    } else if (key == "tick_rate") {
        config.tick_rate_hz = static_cast<uint32_t>(ParseNumber("tick_rate", value, kTickRateRange));
    } else if (key == "threads") {
        config.worker_threads = static_cast<uint32_t>(ParseNumber("threads", value, kThreadsRange));
    } else if (key == "shard_id") {
        config.shard_id = static_cast<uint32_t>(ParseNumber("shard_id", value, kShardRange));
    } else if (key == "log_level") {
        const std::optional<LogLevel> level = ParseLogLevel(value);
        if (!level) {
            throw ConfigError("log_level",
                              "must be debug, info, warn or error, not \"" + std::string(value) + "\"");
        }
        config.log_level = *level;
    } else if (key == "summary_interval") {
        config.summary_interval_s =
            static_cast<uint32_t>(ParseNumber("summary_interval", value, kSummaryRange));
    } else {
        throw ConfigError(std::string(key), "unknown key");
    }
}

ServerConfig ParseConfigText(std::string_view text, ServerConfig base) {
    constexpr std::string_view kBom = "\xEF\xBB\xBF";
    if (text.substr(0, kBom.size()) == kBom) {
        text.remove_prefix(kBom.size());
    }
    std::set<std::string> seen;
    size_t line_number = 0;
    while (!text.empty()) {
        ++line_number;
        const size_t end = text.find('\n');
        const std::string_view line = Trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::string where = "line " + std::to_string(line_number);
        const size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            throw ConfigError(where, "expected key=value, got \"" + std::string(line) + "\"");
        }
        const std::string_view key = Trim(line.substr(0, equals));
        const std::string_view value = Trim(line.substr(equals + 1));
        if (key.empty()) {
            throw ConfigError(where, "the key is empty");
        }
        if (!seen.insert(std::string(key)).second) {
            throw ConfigError(std::string(key), "given more than once");
        }
        ApplySetting(base, key, value);
    }
    return base;
}

ServerConfig LoadConfigFile(const std::filesystem::path& path, ServerConfig base) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream contents;
    if (file) {
        contents << file.rdbuf();
    }
    if (!file || file.bad() || contents.bad()) {
        throw ConfigError("config", "cannot read " + path.string());
    }
    return ParseConfigText(contents.str(), base);
}

ServerConfig ParseArguments(std::span<const std::string_view> args, ServerConfig base) {
    std::set<std::string> seen;
    for (const std::string_view arg : args) {
        const size_t equals = arg.find('=');
        if (arg.substr(0, 2) != "--" || equals == std::string_view::npos) {
            throw ConfigError(std::string(arg), "expected --key=value");
        }
        const std::string_view key = arg.substr(2, equals - 2);
        if (key.empty()) {
            throw ConfigError(std::string(arg), "the key is empty");
        }
        if (!seen.insert(std::string(key)).second) {
            throw ConfigError(std::string(key), "given more than once");
        }
        ApplySetting(base, key, arg.substr(equals + 1));
    }
    return base;
}

ServerConfig ResolveConfig(std::span<const std::string_view> args) {
    constexpr std::string_view kConfigPrefix = "--config=";
    std::vector<std::string_view> rest;
    std::optional<std::string_view> config_path;
    for (const std::string_view arg : args) {
        if (arg.substr(0, kConfigPrefix.size()) == kConfigPrefix) {
            if (config_path) {
                throw ConfigError("config", "given more than once");
            }
            config_path = arg.substr(kConfigPrefix.size());
            if (config_path->empty()) {
                throw ConfigError("config", "missing value");
            }
        } else {
            rest.push_back(arg);
        }
    }
    ServerConfig config;
    if (config_path) {
        config = LoadConfigFile(std::filesystem::path(std::string(*config_path)), config);
    }
    config = ParseArguments(rest, config);
    Validate(config);
    return config;
}

std::string ConfigHelp() {
    const ServerConfig defaults;
    std::ostringstream text;
    text << "Usage: tessera_example_server [--config=<file>] [--<key>=<value> ...]\n"
         << "\n"
         << "Settings can come from a file of key=value lines (# starts a comment) and from the command\n"
         << "line, where a setting overrides the file. A key may appear once per source.\n"
         << "\n"
         << "  port              UDP port to listen on, 1 to 65535 (default " << defaults.port << ")\n"
         << "  max_players       players at once, 1 to 65535 (default " << defaults.max_players << ")\n"
         << "  tick_rate         ticks per second, 1 to 1000 (default " << defaults.tick_rate_hz << ")\n"
         << "  threads           threads for parallel work, 1 to 256 (default " << defaults.worker_threads
         << ")\n"
         << "  shard_id          number written into every entity id, 0 to 4095 (default "
         << defaults.shard_id << ")\n"
         << "  log_level         debug, info, warn or error (default " << LogLevelName(defaults.log_level)
         << ")\n"
         << "  summary_interval  seconds between summary lines, 0 to 3600; 0 turns them off (default "
         << defaults.summary_interval_s << ")\n";
    return text.str();
}

} // namespace tessera::server
