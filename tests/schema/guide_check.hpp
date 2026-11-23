// SPDX-License-Identifier: Apache-2.0
// Checks a guide of the documentation against the files of the repository, so that what the guide shows is
// what is there. Every block of code in the guide says where it comes from, in an HTML comment right before
// it:
//   <!-- source: path/in/the/repository -->   the lines of the block are lines of that file, in a row and
//   equal
//                                              (a line that is only "..." stands for any lines in between)
//   <!-- output: arguments -->                the block is what `tessera_schema <arguments>` prints
//                                              (the path of the repository is left out of what it prints)
//   <!-- shell -->                             commands to type: they come from no file, and are not compared
// The text outside the blocks quotes no long number and no hexadecimal number (they come from the blocks),
// and every relative link points to a file or folder that exists.
#pragma once

#include "tessera/schema/cli.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace guide_check {

/// How many blocks of each sort the guide has.
struct Stats {
    size_t sources = 0;
    size_t outputs = 0;
    size_t shells = 0;
};

namespace detail {

inline std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string line;
    for (const char c : text) {
        if (c == '\n') {
            lines.push_back(line);
            line.clear();
        } else if (c != '\r') {
            line += c;
        }
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    return lines;
}

inline std::string TrimRight(std::string text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.pop_back();
    }
    return text;
}

inline std::string Trim(const std::string& text) {
    std::string result = TrimRight(text);
    size_t start = 0;
    while (start < result.size() && (result[start] == ' ' || result[start] == '\t')) {
        ++start;
    }
    return result.substr(start);
}

inline std::optional<std::string> ReadText(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

struct Marker {
    std::string kind; // "source", "output" or "shell"
    std::string argument;
    size_t line = 0;
};

// The marker in a line that is only an HTML comment, if it is one of ours.
inline std::optional<Marker> ParseMarker(const std::string& line, size_t number) {
    const std::string text = Trim(line);
    if (text.rfind("<!--", 0) != 0 || text.size() < 7 || text.compare(text.size() - 3, 3, "-->") != 0) {
        return std::nullopt;
    }
    const std::string inner = Trim(text.substr(4, text.size() - 7));
    Marker marker;
    marker.line = number;
    if (inner == "shell") {
        marker.kind = "shell";
        return marker;
    }
    for (const char* kind : {"source:", "output:"}) {
        const std::string prefix = kind;
        if (inner.rfind(prefix, 0) == 0) {
            marker.kind = prefix.substr(0, prefix.size() - 1);
            marker.argument = Trim(inner.substr(prefix.size()));
            return marker;
        }
    }
    return std::nullopt;
}

inline bool IsHtmlComment(const std::string& line) {
    const std::string text = Trim(line);
    return text.rfind("<!--", 0) == 0 && text.size() >= 7 && text.compare(text.size() - 3, 3, "-->") == 0;
}

// Looks for `part` (lines in a row) in `file` from line `from`. Returns the line where it starts, or npos.
inline size_t FindRun(const std::vector<std::string>& file, const std::vector<std::string>& part,
                      size_t from) {
    if (part.empty()) {
        return from;
    }
    for (size_t start = from; start + part.size() <= file.size(); ++start) {
        bool same = true;
        for (size_t i = 0; i < part.size() && same; ++i) {
            same = file[start + i] == part[i];
        }
        if (same) {
            return start;
        }
    }
    return std::string::npos;
}

inline void CheckSource(const Marker& marker, const std::vector<std::string>& block, size_t block_line,
                        const std::string& repository_dir, std::vector<std::string>& problems) {
    const std::string where = "line " + std::to_string(block_line);
    const std::optional<std::string> text = ReadText(std::filesystem::path(repository_dir) / marker.argument);
    if (!text) {
        problems.push_back("[source] " + where + ": the file " + marker.argument + " does not exist");
        return;
    }
    std::vector<std::string> file = SplitLines(*text);
    for (std::string& line : file) {
        line = TrimRight(line);
    }
    // The block, in parts that a line of "..." tells apart.
    std::vector<std::vector<std::string>> parts(1);
    for (const std::string& raw : block) {
        const std::string line = TrimRight(raw);
        if (Trim(line) == "...") {
            parts.emplace_back();
        } else {
            parts.back().push_back(line);
        }
    }
    size_t from = 0;
    for (const std::vector<std::string>& part : parts) {
        const size_t found = FindRun(file, part, from);
        if (found == std::string::npos) {
            // Say the first line that is not in the file at all, or else the first one of the part.
            std::string culprit = part.front();
            for (const std::string& line : part) {
                if (FindRun(file, {line}, 0) == std::string::npos) {
                    culprit = line;
                    break;
                }
            }
            problems.push_back("[source] " + where + ": these lines are not in " + marker.argument +
                               ", in a row and in this order; look at: " + culprit);
            return;
        }
        from = found + part.size();
    }
}

inline void CheckOutput(const Marker& marker, const std::vector<std::string>& block, size_t block_line,
                        const std::string& repository_dir, std::vector<std::string>& problems) {
    const std::string where = "line " + std::to_string(block_line);
    std::vector<std::string> args;
    std::istringstream words(marker.argument);
    std::string word;
    while (words >> word) {
        const std::filesystem::path in_repository = std::filesystem::path(repository_dir) / word;
        std::error_code error;
        if (word.rfind("-", 0) != 0 && std::filesystem::exists(in_repository, error)) {
            word = in_repository.generic_string();
        }
        args.push_back(word);
    }
    std::ostringstream out;
    std::ostringstream err;
    const int code = tessera::schema::Run(args, out, err);
    if (code != 0) {
        problems.push_back("[output] " + where + ": `tessera_schema " + marker.argument + "` exits with " +
                           std::to_string(code));
        return;
    }
    std::string printed = out.str();
    const std::string root = std::filesystem::path(repository_dir).generic_string() + "/";
    for (size_t at = printed.find(root); at != std::string::npos; at = printed.find(root, at)) {
        printed.erase(at, root.size());
    }
    std::string shown;
    for (const std::string& line : block) {
        shown += line + "\n";
    }
    if (printed != shown) {
        problems.push_back("[output] " + where + ": the block is not what `tessera_schema " +
                           marker.argument + "` prints; it prints:\n" + printed);
    }
}

inline bool HasLongNumber(const std::string& line) {
    size_t run = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        run = (c >= '0' && c <= '9') ? run + 1 : 0;
        if (run > 5) {
            return true;
        }
    }
    return false;
}

inline bool HasHexNumber(const std::string& line) {
    for (size_t i = 0; i + 2 < line.size(); ++i) {
        if (line[i] == '0' && (line[i + 1] == 'x' || line[i + 1] == 'X')) {
            const char next = line[i + 2];
            if ((next >= '0' && next <= '9') || (next >= 'a' && next <= 'f') ||
                (next >= 'A' && next <= 'F')) {
                return true;
            }
        }
    }
    return false;
}

// The targets of the links of a line of text: what is between "](" and ")".
inline std::vector<std::string> LinkTargets(const std::string& line) {
    std::vector<std::string> targets;
    for (size_t at = line.find("]("); at != std::string::npos; at = line.find("](", at + 2)) {
        const size_t end = line.find(')', at + 2);
        if (end == std::string::npos) {
            break;
        }
        targets.push_back(line.substr(at + 2, end - at - 2));
    }
    return targets;
}

} // namespace detail

/// Checks `markdown`, a guide of the folder `guide_folder` (from the repository), against the files of the
/// repository at `repository_dir`. Returns what is wrong, one text for each problem, each starting with its
/// kind
/// ("[marker]", "[source]", "[output]", "[prose]" or "[link]") and the line of the guide.
inline std::vector<std::string> CheckGuide(const std::string& markdown, const std::string& repository_dir,
                                           Stats* stats = nullptr,
                                           const std::string& guide_folder = "docs/guides") {
    using namespace detail;
    std::vector<std::string> problems;
    const std::vector<std::string> lines = SplitLines(markdown);
    std::optional<Marker> pending;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        const size_t number = i + 1;
        if (Trim(line).rfind("```", 0) == 0) {
            std::vector<std::string> block;
            size_t end = i + 1;
            while (end < lines.size() && Trim(lines[end]).rfind("```", 0) != 0) {
                block.push_back(lines[end]);
                ++end;
            }
            if (end >= lines.size()) {
                problems.push_back("[marker] line " + std::to_string(number) +
                                   ": this block of code is never closed");
                return problems;
            }
            if (!pending) {
                problems.push_back(
                    "[marker] line " + std::to_string(number) +
                    ": this block of code does not say where it comes from (put <!-- source: path -->, "
                    "<!-- output: arguments --> or <!-- shell --> right before it)");
            } else if (pending->kind == "source") {
                if (stats) {
                    ++stats->sources;
                }
                CheckSource(*pending, block, number, repository_dir, problems);
            } else if (pending->kind == "output") {
                if (stats) {
                    ++stats->outputs;
                }
                CheckOutput(*pending, block, number, repository_dir, problems);
            } else if (stats) {
                ++stats->shells;
            }
            pending.reset();
            i = end;
            continue;
        }
        if (const std::optional<Marker> marker = ParseMarker(line, number)) {
            pending = marker;
            continue;
        }
        if (Trim(line).empty() || IsHtmlComment(line)) {
            continue;
        }
        pending.reset();
        if (HasLongNumber(line) || HasHexNumber(line)) {
            problems.push_back(
                "[prose] line " + std::to_string(number) +
                ": the text quotes a long or hexadecimal number; show it in a block that comes from a file");
        }
        for (const std::string& target : LinkTargets(line)) {
            if (target.empty() || target[0] == '#' || target.find("://") != std::string::npos ||
                target.rfind("mailto:", 0) == 0) {
                continue;
            }
            const std::string path = target.substr(0, target.find('#'));
            std::error_code error;
            if (!std::filesystem::exists(std::filesystem::path(repository_dir) / guide_folder / path,
                                         error)) {
                problems.push_back("[link] line " + std::to_string(number) + ": " + target +
                                   " points to nothing");
            }
        }
    }
    return problems;
}

} // namespace guide_check
