// SPDX-License-Identifier: Apache-2.0
#include "tessera/server/server_config.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace tessera::server;

namespace {

// The error a call throws, if it throws one; a different kind of exception is not caught on purpose.
template <class F> std::optional<ConfigError> Failure(F&& call) {
    try {
        call();
    } catch (const ConfigError& error) {
        return error;
    }
    return std::nullopt;
}

bool FailsOn(std::string_view text, std::string_view key) {
    const auto error = Failure([&] { ParseConfigText(text); });
    return error && error->Key() == key;
}

ServerConfig Resolve(const std::vector<std::string_view>& args) {
    return ResolveConfig(args);
}

constexpr std::string_view kEveryKey = "port=9000\n"
                                       "max_players=64\n"
                                       "tick_rate=60\n"
                                       "threads=4\n"
                                       "shard_id=12\n"
                                       "log_level=debug\n"
                                       "summary_interval=5\n";

void CheckEveryKey(const ServerConfig& config) {
    TESSERA_CHECK(config.port == 9000);
    TESSERA_CHECK(config.max_players == 64);
    TESSERA_CHECK(config.tick_rate_hz == 60);
    TESSERA_CHECK(config.worker_threads == 4);
    TESSERA_CHECK(config.shard_id == 12);
    TESSERA_CHECK(config.log_level == LogLevel::Debug);
    TESSERA_CHECK(config.summary_interval_s == 5);
}

} // namespace

TESSERA_TEST(config_defaults_are_valid) {
    const ServerConfig config;
    TESSERA_CHECK(!Failure([&] { Validate(config); }));
    TESSERA_CHECK(config.port == 7777);
    TESSERA_CHECK(config.max_players == 128);
    TESSERA_CHECK(config.tick_rate_hz == 30);
    TESSERA_CHECK(config.worker_threads == 1);
    TESSERA_CHECK(config.shard_id == 0);
    TESSERA_CHECK(config.log_level == LogLevel::Info);
    TESSERA_CHECK(config.summary_interval_s == 0);
}

TESSERA_TEST(config_reads_every_key) {
    CheckEveryKey(ParseConfigText(kEveryKey));
}

TESSERA_TEST(config_ignores_comments_blank_lines_and_spaces_around_keys) {
    const ServerConfig config = ParseConfigText("# the server\n"
                                                "\n"
                                                "   port =  9000  \n"
                                                "\t# another comment\n"
                                                "max_players=64\n"
                                                "  tick_rate\t=\t60\n"
                                                "threads=4\n"
                                                "shard_id = 12\n"
                                                "log_level = debug\n"
                                                "\n"
                                                "summary_interval=5");
    CheckEveryKey(config);
}

TESSERA_TEST(config_accepts_crlf_line_endings) {
    std::string crlf;
    for (const char c : kEveryKey) {
        if (c == '\n') {
            crlf += '\r';
        }
        crlf += c;
    }
    const ServerConfig with_crlf = ParseConfigText(crlf);
    CheckEveryKey(with_crlf);
    const ServerConfig with_lf = ParseConfigText(kEveryKey);
    TESSERA_CHECK(with_crlf.port == with_lf.port && with_crlf.log_level == with_lf.log_level &&
                  with_crlf.summary_interval_s == with_lf.summary_interval_s);
}

TESSERA_TEST(config_strips_a_utf8_byte_order_mark) {
    const std::string with_bom = std::string("\xEF\xBB\xBF") + std::string(kEveryKey);
    CheckEveryKey(ParseConfigText(with_bom));
    // The mark counts as nothing: a comment right after it is still a comment, and line numbers do not move.
    TESSERA_CHECK(!Failure([] { ParseConfigText("\xEF\xBB\xBF# comment\nport=9000\n"); }));
    const auto error = Failure([] { ParseConfigText("\xEF\xBB\xBFport=9000\nnonsense\n"); });
    TESSERA_CHECK(error && error->Key() == "line 2");
}

TESSERA_TEST(config_argument_overrides_file_and_keeps_the_other_keys) {
    ServerConfig from_file = ParseConfigText("port=9000\nmax_players=64\nshard_id=3\n");
    const std::vector<std::string_view> args = {"--port=9100", "--log_level=warn"};
    const ServerConfig config = ParseArguments(args, from_file);
    TESSERA_CHECK(config.port == 9100);       // the argument wins
    TESSERA_CHECK(config.max_players == 64);  // the file's, untouched
    TESSERA_CHECK(config.shard_id == 3);      // the file's, untouched
    TESSERA_CHECK(config.log_level == LogLevel::Warn);
    TESSERA_CHECK(config.tick_rate_hz == 30); // the default, untouched
}

TESSERA_TEST(config_unknown_key_is_an_error_naming_it) {
    const auto in_text = Failure([] { ParseConfigText("port=9000\ncolour=red\n"); });
    TESSERA_CHECK(in_text && in_text->Key() == "colour");
    TESSERA_CHECK(in_text && std::string(in_text->what()).find("colour") != std::string::npos);
    const std::vector<std::string_view> args = {"--colour=red"};
    const auto in_args = Failure([&] { ParseArguments(args); });
    TESSERA_CHECK(in_args && in_args->Key() == "colour");
    const auto in_setting = Failure([] {
        ServerConfig config;
        ApplySetting(config, "colour", "red");
    });
    TESSERA_CHECK(in_setting && in_setting->Key() == "colour");
}

TESSERA_TEST(config_line_without_equals_is_an_error_naming_the_line) {
    const auto error = Failure([] { ParseConfigText("# header\nport=9000\njust some words\nthreads=2\n"); });
    TESSERA_CHECK(error && error->Key() == "line 3");
    TESSERA_CHECK(error && std::string(error->what()).find("just some words") != std::string::npos);
    const std::vector<std::string_view> no_dashes = {"port=9000"};
    const auto bad_argument = Failure([&] { ParseArguments(no_dashes); });
    TESSERA_CHECK(bad_argument && bad_argument->Key() == "port=9000");
    const std::vector<std::string_view> no_equals = {"--port"};
    const auto missing_equals = Failure([&] { ParseArguments(no_equals); });
    TESSERA_CHECK(missing_equals && missing_equals->Key() == "--port");
}

TESSERA_TEST(config_empty_key_or_empty_value_is_an_error) {
    const auto empty_key = Failure([] { ParseConfigText("port=9000\n=5\n"); });
    TESSERA_CHECK(empty_key && empty_key->Key() == "line 2");
    const auto empty_value = Failure([] { ParseConfigText("port=\n"); });
    TESSERA_CHECK(empty_value && empty_value->Key() == "port");
    const auto blank_value = Failure([] { ParseConfigText("log_level =   \n"); });
    TESSERA_CHECK(blank_value && blank_value->Key() == "log_level");
    const std::vector<std::string_view> key_missing = {"--=5"};
    TESSERA_CHECK(Failure([&] { ParseArguments(key_missing); }));
    const std::vector<std::string_view> value_missing = {"--port="};
    const auto error = Failure([&] { ParseArguments(value_missing); });
    TESSERA_CHECK(error && error->Key() == "port");
}

TESSERA_TEST(config_duplicate_key_in_one_source_is_an_error) {
    const auto in_text = Failure([] { ParseConfigText("port=9000\nthreads=2\nport=9001\n"); });
    TESSERA_CHECK(in_text && in_text->Key() == "port");
    const std::vector<std::string_view> args = {"--threads=2", "--threads=3"};
    const auto in_args = Failure([&] { ParseArguments(args); });
    TESSERA_CHECK(in_args && in_args->Key() == "threads");
    // The same key in two sources is how an override works, and stays allowed.
    const std::vector<std::string_view> override_args = {"--threads=3"};
    TESSERA_CHECK(!Failure([&] { ParseArguments(override_args, ParseConfigText("threads=2\n")); }));
}

TESSERA_TEST(config_rejects_port_0_and_65536) {
    TESSERA_CHECK(FailsOn("port=0", "port"));
    TESSERA_CHECK(FailsOn("port=65536", "port"));
    TESSERA_CHECK(ParseConfigText("port=1").port == 1);
    TESSERA_CHECK(ParseConfigText("port=65535").port == 65535);
    ServerConfig zero;
    zero.port = 0;
    const auto error = Failure([&] { Validate(zero); });
    TESSERA_CHECK(error && error->Key() == "port");
}

TESSERA_TEST(config_rejects_tick_rate_0_and_1001) {
    TESSERA_CHECK(FailsOn("tick_rate=0", "tick_rate"));
    TESSERA_CHECK(FailsOn("tick_rate=1001", "tick_rate"));
    TESSERA_CHECK(ParseConfigText("tick_rate=1").tick_rate_hz == 1);
    TESSERA_CHECK(ParseConfigText("tick_rate=1000").tick_rate_hz == 1000);
    ServerConfig config;
    config.tick_rate_hz = 1001;
    const auto error = Failure([&] { Validate(config); });
    TESSERA_CHECK(error && error->Key() == "tick_rate"); // the text key, not the field's name
}

TESSERA_TEST(config_rejects_max_players_0_and_65536) {
    TESSERA_CHECK(FailsOn("max_players=0", "max_players"));
    TESSERA_CHECK(FailsOn("max_players=65536", "max_players"));
    TESSERA_CHECK(ParseConfigText("max_players=1").max_players == 1);
    TESSERA_CHECK(ParseConfigText("max_players=65535").max_players == 65535);
    ServerConfig config;
    config.max_players = 65536;
    const auto error = Failure([&] { Validate(config); });
    TESSERA_CHECK(error && error->Key() == "max_players");
}

TESSERA_TEST(config_rejects_threads_0_and_257) {
    TESSERA_CHECK(FailsOn("threads=0", "threads"));
    TESSERA_CHECK(FailsOn("threads=257", "threads"));
    TESSERA_CHECK(ParseConfigText("threads=1").worker_threads == 1);
    TESSERA_CHECK(ParseConfigText("threads=256").worker_threads == 256);
    ServerConfig config;
    config.worker_threads = 0;
    const auto error = Failure([&] { Validate(config); });
    TESSERA_CHECK(error && error->Key() == "threads");
}

TESSERA_TEST(config_rejects_shard_id_4096_and_accepts_4095) {
    TESSERA_CHECK(FailsOn("shard_id=4096", "shard_id"));
    TESSERA_CHECK(ParseConfigText("shard_id=4095").shard_id == 4095);
    TESSERA_CHECK(ParseConfigText("shard_id=0").shard_id == 0);
    ServerConfig config;
    config.shard_id = 4096;
    const auto error = Failure([&] { Validate(config); });
    TESSERA_CHECK(error && error->Key() == "shard_id");
    TESSERA_CHECK(FailsOn("summary_interval=3601", "summary_interval"));
    TESSERA_CHECK(ParseConfigText("summary_interval=3600").summary_interval_s == 3600);
}

TESSERA_TEST(config_rejects_values_that_are_not_plain_numbers_or_levels) {
    TESSERA_CHECK(FailsOn("port=80x", "port"));
    TESSERA_CHECK(FailsOn("port=-1", "port"));
    TESSERA_CHECK(FailsOn("tick_rate=1e3", "tick_rate"));
    TESSERA_CHECK(FailsOn("threads=+2", "threads"));
    TESSERA_CHECK(FailsOn("threads=2.0", "threads"));
    TESSERA_CHECK(FailsOn("threads=0x10", "threads"));
    TESSERA_CHECK(FailsOn("max_players=99999999999999999999999", "max_players"));
    TESSERA_CHECK(FailsOn("log_level=loud", "log_level"));
    TESSERA_CHECK(FailsOn("log_level=INFO", "log_level"));
    // Spaces inside an argument are part of the value, so the value is not a number.
    const std::vector<std::string_view> space = {"--port= 7"};
    const auto error = Failure([&] { ParseArguments(space); });
    TESSERA_CHECK(error && error->Key() == "port");
    const std::vector<std::string_view> loud = {"--log_level=loud"};
    const auto level = Failure([&] { ParseArguments(loud); });
    TESSERA_CHECK(level && level->Key() == "log_level");
}

TESSERA_TEST(config_missing_file_names_the_path) {
    const std::filesystem::path missing =
        std::filesystem::temp_directory_path() / "tessera_server_no_such_file.cfg";
    const auto error = Failure([&] { LoadConfigFile(missing); });
    TESSERA_CHECK(error && error->Key() == "config");
    TESSERA_CHECK(error && std::string(error->what()).find(missing.string()) != std::string::npos);
    const std::string argument = "--config=" + missing.string();
    const std::vector<std::string_view> args = {argument};
    const auto resolved = Failure([&] { ResolveConfig(args); });
    TESSERA_CHECK(resolved && resolved->Key() == "config");
}

TESSERA_TEST(config_file_from_disk_with_an_argument_on_top) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("tessera_server_config_" + std::to_string(stamp) + ".cfg");
    {
        std::ofstream file(path, std::ios::binary);
        file << "# a server\r\nport=9000\r\nmax_players=64\r\nshard_id=12\r\n";
    }
    const std::string config_argument = "--config=" + path.string();
    const std::vector<std::string_view> args = {"--port=9100", config_argument, "--tick_rate=60"};
    ServerConfig config;
    const auto error = Failure([&] { config = Resolve(args); });
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    TESSERA_CHECK(!error);
    TESSERA_CHECK(config.port == 9100);      // the argument, over the file's 9000
    TESSERA_CHECK(config.max_players == 64); // from the file
    TESSERA_CHECK(config.shard_id == 12);    // from the file
    TESSERA_CHECK(config.tick_rate_hz == 60);
    TESSERA_CHECK(config.worker_threads == 1);
    // The settings are checked once everything has been read.
    const std::vector<std::string_view> bad = {"--port=0"};
    const auto bad_port = Failure([&] { Resolve(bad); });
    TESSERA_CHECK(bad_port && bad_port->Key() == "port");
}
