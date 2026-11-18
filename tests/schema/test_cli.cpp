// SPDX-License-Identifier: Apache-2.0
#include "schema_test_support.hpp"
#include "tessera/schema/cli.hpp"
#include "tessera/schema/cpp_generator.hpp"
#include "tessera/schema/schema_check.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace tessera::schema;
using schema_test::At;
using schema_test::Has;
using schema_test::ReadAll;
using schema_test::ReadTestFile;
using schema_test::TempDir;

namespace {

namespace fs = std::filesystem;

struct Outcome {
    int code = -1;
    std::string out;
    std::string err;
};

Outcome Call(const std::vector<std::string>& args) {
    std::ostringstream out;
    std::ostringstream err;
    Outcome outcome;
    outcome.code = Run(args, out, err);
    outcome.out = out.str();
    outcome.err = err.str();
    return outcome;
}

std::string ExampleFile() {
    return std::string(TESSERA_SCHEMA_TEST_DIR) + "/fixtures/example.json";
}

// A file with a mistake in the name and two in the properties.
const char* const kBadSchema = "{\"format_version\": 1,\n"
                               " \"types\": [{\"type\": 1, \"name\": \"Bad\", \"properties\": [\n"
                               "  {\"name\": \"a\", \"kind\": \"flag\", \"bits\": 1},\n"
                               "  {\"name\": \"b\", \"kind\": \"mask\"}]}]}";

} // namespace

TESSERA_TEST(run_without_arguments_prints_usage_and_returns_2) {
    const Outcome result = Call({});
    TESSERA_CHECK(result.code == 2);
    TESSERA_CHECK(result.err.find("Usage:") != std::string::npos);
    TESSERA_CHECK(result.err.find("check") != std::string::npos);
    TESSERA_CHECK(result.err.find("hash") != std::string::npos);
    TESSERA_CHECK(result.err.find("generate") != std::string::npos);
    TESSERA_CHECK(result.out.empty());
    // --help is not a mistake: the usage goes to the standard output and the code is 0.
    const Outcome help = Call({"--help"});
    TESSERA_CHECK(help.code == 0 && Has(help.out, "Usage:") && help.err.empty());
}

TESSERA_TEST(unknown_command_and_unknown_option_return_2) {
    const std::vector<std::vector<std::string>> wrong = {
        {"frobnicate"},
        {"generate", "--cpp"},
        {"generate", "--cpp", "out.hpp"},
        {"generate", "--namespace"},
        {"generate", "--cpp", "out.hpp", "--bogus", "schema.json"},
        {"generate", "--cpp", "out.hpp", "a.json", "b.json"},
        {"generate", "schema.json"},
        {"check"},
        {"check", "a.json", "b.json"},
        {"check", "--bogus"},
        {"hash"},
        {"hash", "--bogus"},
        // A flag is not a value: --cpp with nothing after it must not take --namespace as the path.
        {"generate", "--cpp", "--namespace", "x", "schema.json"},
        {"generate", "--cpp", "out.hpp", "--namespace", "--cpp", "schema.json"},
    };
    for (const std::vector<std::string>& args : wrong) {
        const Outcome result = Call(args);
        TESSERA_CHECK(result.code == 2);
        TESSERA_CHECK(Has(result.err, "Usage:"));
        TESSERA_CHECK(result.out.empty());
        if (result.code != 2) {
            std::printf("    for \"%s\" ...\n", args[0].c_str());
        }
    }
    TESSERA_CHECK(Has(Call({"frobnicate"}).err, "unknown command frobnicate"));
    TESSERA_CHECK(Has(Call({"check", "--bogus"}).err, "unknown option --bogus"));
    TESSERA_CHECK(Has(Call({"generate", "--cpp", "--namespace", "x", "schema.json"}).err, "--cpp needs a value"));
    TESSERA_CHECK(Has(Call({"generate", "--cpp", "o.hpp", "--namespace", "--cpp", "schema.json"}).err, "--namespace needs a value"));
}

TESSERA_TEST(check_prints_ok_with_counts_and_hash) {
    const Outcome result = Call({"check", ExampleFile()});
    TESSERA_CHECK(result.code == 0);
    TESSERA_CHECK(result.out == ExampleFile() + ": OK, 2 types, hash 0x04b6ebf2e86202d0\n");
    TESSERA_CHECK(result.err.empty());
    // A single type is "1 type".
    TempDir dir;
    const std::string one =
        dir.Write("one.json", "{\"format_version\": 1, \"types\": [{\"type\": 1, \"name\": \"e\", "
                              "\"properties\": [{\"name\": \"alive\", \"kind\": \"flag\"}]}]}");
    const Outcome single = Call({"check", one});
    TESSERA_CHECK(single.code == 0 && Has(single.out, ": OK, 1 type, hash 0x"));
}

TESSERA_TEST(check_prints_every_diagnostic_with_file_line_column_and_fix) {
    TempDir dir;
    const std::string path = dir.Write("bad.json", kBadSchema);
    const Outcome result = Call({"check", path});
    TESSERA_CHECK(result.code == 1);
    const std::string text = kBadSchema;
    const auto at = [&](const std::string& needle) {
        const auto place = At(text, needle);
        return path + ":" + std::to_string(place.line) + ":" + std::to_string(place.column) + ": error: ";
    };
    const std::string expected =
        at("\"Bad\"") +
        "\"Bad\" is not a valid name\n"
        "  fix: use lowercase letters, digits and underscores, and start each part with a letter, for "
        "example "
        "\"max_health\"\n" +
        at("\"bits\"") +
        "the key \"bits\" does not belong to the kind \"flag\"\n"
        "  fix: remove it; a \"flag\" property takes no keys besides \"name\" and \"note\"\n" +
        at("{\"name\": \"b\"") +
        "a property of the kind \"mask\" needs the key \"bits\"\n"
        "  fix: add \"bits\": a whole number\n";
    TESSERA_CHECK(result.err == expected);
    if (result.err != expected) {
        std::printf("--- expected\n%s--- got\n%s", expected.c_str(), result.err.c_str());
    }
}

TESSERA_TEST(check_says_how_many_problems_it_does_not_show) {
    TempDir dir;
    std::string property = R"({"name": "p", "kind": "flag")";
    for (int i = 0; i < 24; ++i) {
        property += ", \"extra" + std::to_string(i) + "_key\": 1";
    }
    const std::string text =
        R"({"format_version": 1, "types": [{"type": 1, "name": "t", "properties": [)" + property + "}]}]}";
    const std::string path = dir.Write("many.json", text);
    const Outcome result = Call({"check", path});
    TESSERA_CHECK(result.code == 1);
    TESSERA_CHECK(Has(result.err, path + ": and 4 more problems are not shown\n"));
    size_t lines = 0;
    for (const char c : result.err) {
        lines += c == '\n' ? 1 : 0;
    }
    TESSERA_CHECK(lines == 21); // 20 problems (none has a correction), and the last line
}

TESSERA_TEST(check_returns_1_and_prints_nothing_on_stdout_for_a_bad_schema) {
    TempDir dir;
    const Outcome broken = Call({"check", dir.Write("broken.json", "{\"format_version\": 1,}")});
    TESSERA_CHECK(broken.code == 1 && broken.out.empty());
    TESSERA_CHECK(
        Has(broken.err,
            "broken.json:1:21: error: there is a comma after the last member\n  fix: remove the comma"));
    // What the replication code refuses is refused with its words, and a place.
    const std::string text =
        "{\"format_version\": 1, \"types\": [{\"type\": 1, \"name\": \"entity\", \"properties\": [\n"
        "{\"name\": \"position\", \"kind\": \"position\", \"step\": 0, \"range\": 10}]}]}";
    const Outcome core = Call({"check", dir.Write("core.json", text)});
    TESSERA_CHECK(core.code == 1 && core.out.empty());
    TESSERA_CHECK(Has(core.err, "core.json:2:1: error: type \"entity\" (1): property 0: position step must "
                                "be a positive, finite number"));
}

static_assert(339718250426401488ull == 0x04b6ebf2e86202d0ull, "the hex of the hash of the example");

TESSERA_TEST(hash_prints_only_the_hash_in_hex) {
    const Outcome result = Call({"hash", ExampleFile()});
    TESSERA_CHECK(result.code == 0 && result.err.empty());
    TESSERA_CHECK(result.out == "0x04b6ebf2e86202d0\n");
    // It is the hash the replication code computes for the same schema.
    const ReadResult read = ReadSchema(ReadTestFile("fixtures/example.json"));
    TESSERA_CHECK(read.file.has_value());
    if (read.file) {
        char buffer[32];
        std::snprintf(buffer, sizeof buffer, "0x%016llx\n",
                      static_cast<unsigned long long>(Check(*read.file).schema->Hash()));
        TESSERA_CHECK(result.out == buffer);
    }
    TempDir dir;
    const Outcome bad = Call({"hash", dir.Write("bad.json", kBadSchema)});
    TESSERA_CHECK(bad.code == 1 && bad.out.empty() && !bad.err.empty());
}

TESSERA_TEST(generate_writes_the_file_and_creates_missing_folders) {
    TempDir dir;
    const std::string out = dir.File("a/b/c/example_schema.hpp");
    const Outcome result = Call({"generate", "--cpp", out, "--namespace", "stable::example", ExampleFile()});
    TESSERA_CHECK(result.code == 0 && result.err.empty());
    const std::string text = ReadAll(out);
    TESSERA_CHECK(Has(text, "// Generated by tessera_schema from example.json. Do not edit"));
    TESSERA_CHECK(Has(text, "\nnamespace stable::example {\n"));
    TESSERA_CHECK(Has(text, "kSchemaHash = 0x04b6ebf2e86202d0ull;"));
    TESSERA_CHECK(Has(text, "inline constexpr uint32_t kPlayerType = 2;"));
    // The default namespace is game_schema.
    const Outcome plain = Call({"generate", "--cpp", dir.File("plain.hpp"), ExampleFile()});
    TESSERA_CHECK(plain.code == 0 && Has(ReadAll(dir.File("plain.hpp")), "\nnamespace game_schema {\n"));
    // The options may come in any order.
    const Outcome reordered =
        Call({"generate", ExampleFile(), "--namespace", "x", "--cpp", dir.File("x.hpp")});
    TESSERA_CHECK(reordered.code == 0 && Has(ReadAll(dir.File("x.hpp")), "\nnamespace x {\n"));
}

TESSERA_TEST(generate_with_invalid_schema_writes_nothing_and_keeps_an_older_file) {
    TempDir dir;
    // No file before: nothing is made, not even the folder.
    const std::string new_out = dir.File("not_made_yet/out.hpp");
    const Outcome refused = Call({"generate", "--cpp", new_out, dir.Write("bad.json", kBadSchema)});
    TESSERA_CHECK(refused.code == 1 && !refused.err.empty());
    TESSERA_CHECK(!fs::exists(new_out) && !fs::exists(dir.Path() / "not_made_yet"));
    // A file from an earlier good run stays as it was.
    const std::string out = dir.File("out.hpp");
    TESSERA_CHECK(Call({"generate", "--cpp", out, ExampleFile()}).code == 0);
    const std::string before = ReadAll(out);
    TESSERA_CHECK(!before.empty());
    const Outcome again = Call({"generate", "--cpp", out, dir.File("bad.json")});
    TESSERA_CHECK(again.code == 1);
    TESSERA_CHECK(ReadAll(out) == before);
    // The same for a schema the replication code refuses (not only one the reading refuses).
    const std::string zero_step =
        "{\"format_version\": 1, \"types\": [{\"type\": 1, \"name\": \"e\", \"properties\": ["
        "{\"name\": \"p\", \"kind\": \"position\", \"step\": 0, \"range\": 1}]}]}";
    TESSERA_CHECK(Call({"generate", "--cpp", out, dir.Write("zero.json", zero_step)}).code == 1);
    TESSERA_CHECK(ReadAll(out) == before);
}

TESSERA_TEST(generate_refuses_to_overwrite_its_input) {
    TempDir dir;
    const std::string original = ReadTestFile("fixtures/example.json");
    const std::string in = dir.Write("a/schema.json", original);
    const Outcome same = Call({"generate", "--cpp", in, in});
    TESSERA_CHECK(same.code == 2 && Has(same.err, "would overwrite it"));
    TESSERA_CHECK(ReadAll(in) == original);
    // The same file by another way of writing the path.
    const std::string other_way = dir.File("a/../a/schema.json");
    const Outcome indirect = Call({"generate", "--cpp", other_way, in});
    TESSERA_CHECK(indirect.code == 2);
    TESSERA_CHECK(ReadAll(in) == original);
}

TESSERA_TEST(generate_leaves_the_file_alone_when_nothing_changed) {
    TempDir dir;
    const std::string in = dir.Write("schema.json", ReadTestFile("fixtures/example.json"));
    const std::string out = dir.File("out.hpp");
    TESSERA_CHECK(Call({"generate", "--cpp", out, in}).code == 0);
    // Put the modification time in the past, so that a rewrite shows.
    const auto past = fs::file_time_type::clock::now() - std::chrono::hours(5);
    fs::last_write_time(out, past);
    const auto before = fs::last_write_time(out);
    const Outcome unchanged = Call({"generate", "--cpp", out, in});
    TESSERA_CHECK(unchanged.code == 0 && Has(unchanged.out, "already up to date"));
    TESSERA_CHECK(fs::last_write_time(out) == before);
    // With another schema the file is written.
    std::string text = ReadTestFile("fixtures/example.json");
    text.replace(text.find("2048.0"), 6, "1024.0");
    dir.Write("schema.json", text);
    const Outcome changed = Call({"generate", "--cpp", out, in});
    TESSERA_CHECK(changed.code == 0 && Has(changed.out, "written"));
    TESSERA_CHECK(fs::last_write_time(out) != before);
    TESSERA_CHECK(Has(ReadAll(out), "kEntityPositionRange = 1024.0f;"));
}

TESSERA_TEST(missing_or_unreadable_input_returns_3_with_a_clear_message) {
    TempDir dir;
    const std::string missing = dir.File("nothing_here.json");
    const Outcome absent = Call({"check", missing});
    TESSERA_CHECK(absent.code == 3 && absent.out.empty());
    TESSERA_CHECK(absent.err == missing + ": error: this file does not exist\n");
    // A folder where a file should be.
    fs::create_directories(dir.Path() / "a_folder");
    const Outcome folder = Call({"check", dir.File("a_folder")});
    TESSERA_CHECK(folder.code == 3 && Has(folder.err, "is a folder, not a file"));
    // The same for hash and generate, and nothing is written.
    TESSERA_CHECK(Call({"hash", missing}).code == 3);
    const Outcome gen = Call({"generate", "--cpp", dir.File("o.hpp"), missing});
    TESSERA_CHECK(gen.code == 3 && !fs::exists(dir.Path() / "o.hpp"));
    // Over a mebibyte: the schema is refused, with the limit in the message (the file is not read whole).
    const std::string big = dir.Write("big.json", std::string((1u << 20) + 1, ' '));
    const Outcome large = Call({"check", big});
    TESSERA_CHECK(large.code == 1);
    TESSERA_CHECK(Has(large.err, big + ":1:1: error: the file is larger than 1048576 bytes"));
    // An output that cannot be written (a folder is in the way).
    fs::create_directories(dir.Path() / "taken");
    const Outcome blocked = Call({"generate", "--cpp", dir.File("taken"), ExampleFile()});
    TESSERA_CHECK(blocked.code == 3 && Has(blocked.err, "cannot replace this file"));
}

TESSERA_TEST(generate_writes_through_a_temporary_file_and_leaves_nothing_behind) {
    TempDir dir;
    const std::string out = dir.File("out/example.hpp");
    TESSERA_CHECK(Call({"generate", "--cpp", out, ExampleFile()}).code == 0);
    // Only the header is in the folder: the temporary file is gone.
    size_t files = 0;
    for (const auto& entry : fs::directory_iterator(dir.Path() / "out")) {
        (void)entry;
        ++files;
    }
    TESSERA_CHECK(files == 1);
    // When the file cannot be replaced, the temporary one is removed too, and the old one stays as it was.
    fs::create_directories(dir.Path() / "taken");
    TESSERA_CHECK(Call({"generate", "--cpp", dir.File("taken"), ExampleFile()}).code == 3);
    size_t left = 0;
    for (const auto& entry : fs::directory_iterator(dir.Path() / "taken")) {
        (void)entry;
        ++left;
    }
    TESSERA_CHECK(left == 0);
    size_t beside = 0;
    for (const auto& entry : fs::directory_iterator(dir.Path())) {
        (void)entry;
        ++beside;
    }
    TESSERA_CHECK(beside == 2); // "out" and "taken", and no temporary file next to "taken"
#ifdef _WIN32
    // A file that something else holds open cannot be replaced on Windows, and must come out unchanged. (Writing straight
    // into it would have changed it.)
    const std::string held = dir.Write("held.hpp", "old contents");
    std::ifstream holder(held, std::ios::binary);
    TESSERA_CHECK(holder.is_open());
    const Outcome blocked = Call({"generate", "--cpp", held, ExampleFile()});
    TESSERA_CHECK(blocked.code == 3);
    holder.close();
    TESSERA_CHECK(ReadAll(held) == "old contents");
#endif
}

TESSERA_TEST(generate_rejects_a_bad_namespace_with_exit_2) {
    TempDir dir;
    for (const char* bad : {"class", "1a", "a::", "::a", ""}) {
        const Outcome result =
            Call({"generate", "--cpp", dir.File("o.hpp"), "--namespace", bad, ExampleFile()});
        TESSERA_CHECK(result.code == 2);
        TESSERA_CHECK(Has(result.err, "bad --namespace:"));
        TESSERA_CHECK(!fs::exists(dir.Path() / "o.hpp"));
    }
    TESSERA_CHECK(
        Has(Call({"generate", "--cpp", dir.File("o.hpp"), "--namespace", "class", ExampleFile()}).err,
            "C++ keyword"));
}
