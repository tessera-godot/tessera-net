// SPDX-License-Identifier: Apache-2.0
#include "tessera/schema/cli.hpp"

#include "tessera/schema/cpp_generator.hpp"
#include "tessera/schema/json_reader.hpp"
#include "tessera/schema/schema_check.hpp"
#include "tessera/schema/schema_file.hpp"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <ostream>
#include <string>
#include <vector>

namespace tessera::schema {

namespace {

constexpr int kOk = 0;
constexpr int kBadFile = 1;
constexpr int kBadUsage = 2;
constexpr int kCannotUseFile = 3;

void PrintUsage(std::ostream& stream) {
    stream << "Usage:\n"
           << "  tessera_schema check <schema.json>\n"
           << "  tessera_schema hash <schema.json>\n"
           << "  tessera_schema generate --cpp <output.hpp> [--namespace a::b] <schema.json>\n"
           << "\n"
           << "check     reads the file, checks it as the server does, and says how many types it has and "
              "its hash.\n"
           << "hash      prints the hash of the schema and nothing else.\n"
           << "generate  writes the C++ header of the schema. Nothing is written when the schema is wrong.\n"
           << "\n"
           << "Exit codes: 0 done, 1 the file or the schema is wrong, 2 the command line is wrong, 3 a file\n"
           << "could not be read or written.\n";
}

int UsageError(std::ostream& err, const std::string& message) {
    err << "tessera_schema: " << message << "\n\n";
    PrintUsage(err);
    return kBadUsage;
}

std::string Hex16(uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "0x%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

// Reads a whole file. At most one byte more than the reader accepts is read, so that a huge file is refused
// without being loaded.
// Returns what went wrong, or an empty string.
std::string ReadFile(const std::filesystem::path& path, std::string& text) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return "this file does not exist";
    }
    if (std::filesystem::is_directory(path, error)) {
        return "this is a folder, not a file";
    }
    if (!std::filesystem::is_regular_file(path, error)) {
        return "this is not a regular file";
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return "cannot open this file (maybe you are not allowed to read it)";
    }
    const size_t limit = json::Limits{}.max_bytes + 1;
    text.resize(limit);
    file.read(text.data(), static_cast<std::streamsize>(limit));
    if (file.bad()) {
        return "cannot read this file";
    }
    text.resize(static_cast<size_t>(file.gcount()));
    return {};
}

struct Loaded {
    SchemaFile file;
    CheckResult check;
};

// Reads and checks the file. Returns the exit code, which is 0 when `loaded` is filled in.
int Load(const std::string& path, std::optional<Loaded>& loaded, std::ostream& err) {
    std::string text;
    const std::string problem = ReadFile(std::filesystem::path(path), text);
    if (!problem.empty()) {
        err << path << ": error: " << problem << "\n";
        return kCannotUseFile;
    }
    ReadResult read = ReadSchema(text);
    if (!read.file) {
        for (const Diagnostic& diagnostic : read.diagnostics) {
            err << Format(path, diagnostic) << "\n";
        }
        if (read.omitted > 0) {
            err << path << ": and " << read.omitted << " more problem" << (read.omitted == 1 ? " is" : "s are")
                << " not shown\n";
        }
        return kBadFile;
    }
    CheckResult check = Check(*read.file);
    if (!check.schema) {
        for (const Diagnostic& diagnostic : check.diagnostics) {
            err << Format(path, diagnostic) << "\n";
        }
        return kBadFile;
    }
    loaded = Loaded{std::move(*read.file), std::move(check)};
    return kOk;
}

// The one argument that `check` and `hash` take: the schema file.
bool OneFile(std::span<const std::string> args, std::string& path, std::ostream& err, int& code) {
    if (args.size() != 2) {
        code = UsageError(err, args.size() < 2 ? "this command needs the schema file" : "too many arguments");
        return false;
    }
    if (args[1].rfind("--", 0) == 0) {
        code = UsageError(err, "unknown option " + args[1]);
        return false;
    }
    path = args[1];
    return true;
}

int RunCheck(std::span<const std::string> args, std::ostream& out, std::ostream& err) {
    std::string path;
    int code = kOk;
    if (!OneFile(args, path, err, code)) {
        return code;
    }
    std::optional<Loaded> loaded;
    code = Load(path, loaded, err);
    if (code != kOk) {
        return code;
    }
    const size_t types = loaded->file.types.size();
    out << path << ": OK, " << types << (types == 1 ? " type" : " types") << ", hash "
        << Hex16(loaded->check.hash) << "\n";
    return kOk;
}

int RunHash(std::span<const std::string> args, std::ostream& out, std::ostream& err) {
    std::string path;
    int code = kOk;
    if (!OneFile(args, path, err, code)) {
        return code;
    }
    std::optional<Loaded> loaded;
    code = Load(path, loaded, err);
    if (code != kOk) {
        return code;
    }
    out << Hex16(loaded->check.hash) << "\n";
    return kOk;
}

bool SameBytes(const std::filesystem::path& path, const std::string& text) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return false;
    }
    if (std::filesystem::file_size(path, error) != text.size() || error) {
        return false; // a file of another size is not the same, and is not read
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::string existing(text.size(), '\0');
    file.read(existing.data(), static_cast<std::streamsize>(existing.size()));
    return file.gcount() == static_cast<std::streamsize>(text.size()) && existing == text;
}

int RunGenerate(std::span<const std::string> args, std::ostream& out, std::ostream& err) {
    std::string output;
    std::string input;
    CppOptions options;
    bool has_output = false;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--cpp" || arg == "--namespace") {
            if (i + 1 >= args.size() || args[i + 1].rfind("--", 0) == 0) {
                return UsageError(err, arg + " needs a value");
            }
            (arg == "--cpp" ? output : options.ns) = args[++i];
            has_output = has_output || arg == "--cpp";
        } else if (arg.rfind("--", 0) == 0) {
            return UsageError(err, "unknown option " + arg);
        } else if (input.empty()) {
            input = arg;
        } else {
            return UsageError(err, "too many arguments");
        }
    }
    if (!has_output) {
        return UsageError(err, "generate needs --cpp <output.hpp>");
    }
    if (input.empty()) {
        return UsageError(err, "generate needs the schema file");
    }
    std::string why;
    if (!IsValidNamespace(options.ns, &why)) {
        return UsageError(err, "bad --namespace: " + why);
    }
    std::error_code error;
    if (std::filesystem::exists(input, error) && std::filesystem::equivalent(input, output, error)) {
        return UsageError(err, "the output is the schema file itself, and generate would overwrite it");
    }
    std::optional<Loaded> loaded;
    const int code = Load(input, loaded, err);
    if (code != kOk) {
        return code; // nothing is written, and a file that was there before stays as it was
    }
    options.source_name = std::filesystem::path(input).filename().string();
    const std::string text = GenerateCpp(loaded->file, loaded->check.hash, options);
    const std::filesystem::path target(output);
    if (SameBytes(target, text)) {
        out << output << ": already up to date\n";
        return kOk;
    }
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), error);
        if (error) {
            err << output << ": error: cannot make the folder: " << error.message() << "\n";
            return kCannotUseFile;
        }
    }
    // The text goes to a temporary file next to the target and then takes its place, so that a failure half way never leaves
    // a header that is cut short where a good one was.
    static std::atomic<unsigned> counter{0};
    std::filesystem::path temporary = target;
    temporary += ".tmp" + std::to_string(counter.fetch_add(1)) + "_" + std::to_string(std::random_device{}());
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.flush();
        if (!file) {
            file.close();
            std::filesystem::remove(temporary, error);
            err << output << ": error: cannot write this file\n";
            return kCannotUseFile;
        }
    }
    std::filesystem::rename(temporary, target, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        err << output << ": error: cannot replace this file: " << error.message() << "\n";
        return kCannotUseFile;
    }
    out << output << ": written\n";
    return kOk;
}

} // namespace

int Run(std::span<const std::string> args, std::ostream& out, std::ostream& err) {
    if (args.empty()) {
        PrintUsage(err);
        return kBadUsage;
    }
    const std::string& command = args[0];
    if (command == "--help" || command == "-h" || command == "help") {
        PrintUsage(out);
        return kOk;
    }
    if (command == "check") {
        return RunCheck(args, out, err);
    }
    if (command == "hash") {
        return RunHash(args, out, err);
    }
    if (command == "generate") {
        return RunGenerate(args, out, err);
    }
    return UsageError(err, "unknown command " + command);
}

} // namespace tessera::schema
