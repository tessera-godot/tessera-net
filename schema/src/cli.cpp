// SPDX-License-Identifier: Apache-2.0
#include "tessera/schema/cli.hpp"

#include "tessera/schema/cpp_generator.hpp"
#include "tessera/schema/gdscript_generator.hpp"
#include "tessera/schema/generator_text.hpp"
#include "tessera/schema/json_reader.hpp"
#include "tessera/schema/names.hpp"
#include "tessera/schema/schema_check.hpp"
#include "tessera/schema/schema_file.hpp"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ostream>
#include <random>
#include <string>
#include <vector>

namespace tessera::schema {

namespace {

constexpr int kOk = 0;
constexpr int kBadFile = 1;
constexpr int kBadUsage = 2;
constexpr int kCannotUseFile = 3;

void PrintUsage(std::ostream& stream) {
    stream
        << "Usage:\n"
        << "  tessera_schema check <schema.json>\n"
        << "  tessera_schema hash <schema.json>\n"
        << "  tessera_schema generate [--cpp <output.hpp> [--namespace a::b]]\n"
        << "                          [--gdscript <output.gd> [--gdscript-class Name]] <schema.json>\n"
        << "\n"
        << "check     reads the file, checks it as the server does, and says how many types it has and "
           "its hash.\n"
        << "hash      prints the hash of the schema and nothing else.\n"
        << "generate  writes the C++ header (--cpp) and the GDScript (--gdscript) of the schema; give one or "
           "both.\n"
        << "          Nothing is written when the schema is wrong.\n"
        << "\n"
        << "--namespace        the namespace of the C++ header (default game_schema).\n"
        << "--gdscript-class   the class_name of the script (default GameSchema).\n"
        << "\n"
        << "Exit codes: 0 done, 1 the file or the schema is wrong, 2 the command line is wrong, 3 a file\n"
        << "could not be read or written.\n";
}

int UsageError(std::ostream& err, const std::string& message) {
    err << "tessera_schema: " << message << "\n\n";
    PrintUsage(err);
    return kBadUsage;
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
            err << path << ": and " << read.omitted << " more problem"
                << (read.omitted == 1 ? " is" : "s are") << " not shown\n";
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

// Writes `text` to `output`, unless the file already has exactly these bytes. Returns 0, or 3 when the file
// cannot be written.
int WriteIfChanged(const std::string& output, const std::string& text, std::ostream& out, std::ostream& err) {
    std::error_code error;
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
    // The text goes to a temporary file next to the target and then takes its place, so that a failure half
    // way never leaves a header that is cut short where a good one was.
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

// True when two paths are the same file, also when one of them is not there yet (the paths are compared after
// the folders in them have been resolved).
bool SameFile(const std::string& a, const std::string& b) {
    std::error_code error;
    if (std::filesystem::exists(a, error) && std::filesystem::exists(b, error)) {
        return std::filesystem::equivalent(a, b, error);
    }
    std::string first = std::filesystem::weakly_canonical(a, error).generic_string();
    std::string second = std::filesystem::weakly_canonical(b, error).generic_string();
#ifdef _WIN32
    // Windows does not tell capitals from small letters in a path (for the letters of ASCII, at least).
    for (std::string* path : {&first, &second}) {
        for (char& c : *path) {
            c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }
    }
#endif
    return !error && first == second;
}

int RunGenerate(std::span<const std::string> args, std::ostream& out, std::ostream& err) {
    std::string cpp_output;
    std::string gdscript_output;
    std::string input;
    CppOptions cpp_options;
    GdScriptOptions gdscript_options;
    bool has_cpp = false;
    bool has_gdscript = false;
    bool has_namespace = false;
    bool has_class = false;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--cpp" || arg == "--namespace" || arg == "--gdscript" || arg == "--gdscript-class") {
            if (i + 1 >= args.size() || args[i + 1].rfind("--", 0) == 0) {
                return UsageError(err, arg + " needs a value");
            }
            const bool given = arg == "--cpp"         ? has_cpp
                               : arg == "--namespace" ? has_namespace
                               : arg == "--gdscript"  ? has_gdscript
                                                      : has_class;
            if (given) {
                return UsageError(err, arg + " given twice");
            }
            const std::string& value = args[++i];
            if (arg == "--cpp") {
                cpp_output = value;
                has_cpp = true;
            } else if (arg == "--namespace") {
                cpp_options.ns = value;
                has_namespace = true;
            } else if (arg == "--gdscript") {
                gdscript_output = value;
                has_gdscript = true;
            } else {
                gdscript_options.class_name = value;
                has_class = true;
            }
        } else if (arg.rfind("--", 0) == 0) {
            return UsageError(err, "unknown option " + arg);
        } else if (input.empty()) {
            input = arg;
        } else {
            return UsageError(err, "too many arguments");
        }
    }
    if (!has_cpp && !has_gdscript) {
        return UsageError(err, "generate needs --cpp <output.hpp>, --gdscript <output.gd>, or both");
    }
    if (input.empty()) {
        return UsageError(err, "generate needs the schema file");
    }
    if (has_namespace && !has_cpp) {
        return UsageError(err, "--namespace needs --cpp: it is the namespace of the C++ header");
    }
    if (has_class && !has_gdscript) {
        return UsageError(err, "--gdscript-class needs --gdscript: it is the class of the script");
    }
    std::string why;
    if (has_cpp && !IsValidNamespace(cpp_options.ns, &why)) {
        return UsageError(err, "bad --namespace: " + why);
    }
    if (has_gdscript) {
        std::string fix;
        why = CheckGdScriptClassName(gdscript_options.class_name, &fix);
        if (!why.empty()) {
            return UsageError(err, "bad --gdscript-class: " + why + (fix.empty() ? "" : "\n  fix: " + fix));
        }
    }
    // No output may be the schema file, and the two outputs may not be one file.
    for (const std::string* output : {&cpp_output, &gdscript_output}) {
        if ((output == &cpp_output ? has_cpp : has_gdscript) && SameFile(input, *output)) {
            return UsageError(err, "the output is the schema file itself, and generate would overwrite it");
        }
    }
    if (has_cpp && has_gdscript && SameFile(cpp_output, gdscript_output)) {
        return UsageError(err, "--cpp and --gdscript are the same file (" + cpp_output +
                                   "), and one would overwrite the other");
    }
    std::optional<Loaded> loaded;
    const int code = Load(input, loaded, err);
    if (code != kOk) {
        return code; // nothing is written, and a file that was there before stays as it was
    }
    // Both texts are made before any file is touched.
    const std::string source_name = std::filesystem::path(input).filename().string();
    cpp_options.source_name = source_name;
    gdscript_options.source_name = source_name;
    const std::string cpp_text = has_cpp ? GenerateCpp(loaded->file, loaded->check.hash, cpp_options) : "";
    const std::string gdscript_text =
        has_gdscript ? GenerateGdScript(loaded->file, loaded->check.hash, gdscript_options) : "";
    if (has_cpp) {
        const int written = WriteIfChanged(cpp_output, cpp_text, out, err);
        if (written != kOk) {
            return written;
        }
    }
    if (has_gdscript) {
        // The first file is there now, so two paths for one file that were not told apart before are told
        // apart here (a link, for example). The header stays; the script is not written over it.
        if (has_cpp && SameFile(cpp_output, gdscript_output)) {
            return UsageError(err, "--cpp and --gdscript are the same file (" + cpp_output +
                                       "), and the script would overwrite the header");
        }
        return WriteIfChanged(gdscript_output, gdscript_text, out, err);
    }
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
