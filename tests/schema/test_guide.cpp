// SPDX-License-Identifier: Apache-2.0
// The guide "Describe your entities once" shows code and output: here they are compared with the files of the
// repository that they come from (see guide_check.hpp), and the checker itself is tried on guides made for
// the test.
#include "guide_check.hpp"
#include "schema_test_support.hpp"

#include <cstddef>
#include <string>
#include <vector>

using schema_test::Has;
using schema_test::ReadAll;
using schema_test::ReadTestFile;
using schema_test::TempDir;

namespace {

const char* const kGuidePath = "docs/guides/describe-your-entities-once.md";

std::string RealGuide() {
    return ReadAll(std::string(TESSERA_REPOSITORY_DIR) + "/" + kGuidePath);
}

// The problems of one kind ("[source]", for example) that the checker finds in the real guide, and the kinds
// of blocks that it has.
std::vector<std::string> RealProblems(const std::string& kind, guide_check::Stats* stats = nullptr) {
    std::vector<std::string> found;
    for (const std::string& problem : guide_check::CheckGuide(RealGuide(), TESSERA_REPOSITORY_DIR, stats)) {
        if (problem.rfind(kind, 0) == 0) {
            found.push_back(problem);
        }
    }
    for (const std::string& problem : found) {
        std::printf("    %s\n", problem.c_str());
    }
    return found;
}

size_t Count(const std::vector<std::string>& problems, const std::string& kind) {
    size_t count = 0;
    for (const std::string& problem : problems) {
        count += problem.rfind(kind, 0) == 0 ? 1 : 0;
    }
    return count;
}

// A small repository for the checker: one text file of five lines, the schema of the tests, and a page next
// to the guide.
void MakeRepository(const TempDir& dir) {
    dir.Write("src/a.txt", "one\ntwo   \nthree\nfour\nfive\n");
    dir.Write("schema.json", ReadTestFile("fixtures/example.json"));
    dir.Write("docs/guides/other.md", "# Other\n");
}

std::vector<std::string> Check(const TempDir& dir, const std::string& guide,
                               guide_check::Stats* stats = nullptr) {
    return guide_check::CheckGuide(guide, dir.Path().generic_string(), stats);
}

} // namespace

TESSERA_TEST(guide_every_code_block_says_where_it_comes_from) {
    TESSERA_CHECK(!RealGuide().empty());
    guide_check::Stats stats;
    TESSERA_CHECK(RealProblems("[marker]", &stats).empty());
    // The guide shows files of the repository and the output of the tool, and keeps its commands few.
    TESSERA_CHECK(stats.sources >= 5);
    TESSERA_CHECK(stats.outputs >= 1);
    TESSERA_CHECK(stats.shells >= 1 && stats.shells <= 8);
}

TESSERA_TEST(guide_source_blocks_are_excerpts_of_their_files) {
    TESSERA_CHECK(!RealGuide().empty());
    TESSERA_CHECK(RealProblems("[source]").empty());
}

TESSERA_TEST(guide_output_blocks_are_what_the_tool_prints) {
    TESSERA_CHECK(!RealGuide().empty());
    TESSERA_CHECK(RealProblems("[output]").empty());
}

TESSERA_TEST(guide_prose_quotes_no_numbers_of_its_own) {
    TESSERA_CHECK(!RealGuide().empty());
    TESSERA_CHECK(RealProblems("[prose]").empty());
}

TESSERA_TEST(guide_links_point_to_files_that_exist) {
    TESSERA_CHECK(!RealGuide().empty());
    TESSERA_CHECK(RealProblems("[link]").empty());
}

TESSERA_TEST(guide_check_finds_a_changed_line_in_an_excerpt) {
    TempDir dir;
    MakeRepository(dir);
    // Lines in a row, with "..." for what is skipped, and a space at the end of a line that does not matter.
    guide_check::Stats stats;
    const std::string good = "<!-- source: src/a.txt -->\n```text\none\ntwo\n...\nfour\nfive\n```\n";
    TESSERA_CHECK(Check(dir, good, &stats).empty());
    TESSERA_CHECK(stats.sources == 1);
    // A blank line between the marker and the block is allowed.
    TESSERA_CHECK(Check(dir, "<!-- source: src/a.txt -->\n\n```text\nthree\n```\n").empty());
    // A changed line, lines that are not in a row, and parts in the wrong order are all found.
    const std::vector<std::string> changed =
        Check(dir, "<!-- source: src/a.txt -->\n```text\none\n2\n...\nfive\n```\n");
    TESSERA_CHECK(changed.size() == 1 && changed[0].rfind("[source] line 2", 0) == 0 &&
                  Has(changed[0], "look at: 2"));
    TESSERA_CHECK(Check(dir, "<!-- source: src/a.txt -->\n```text\none\nthree\n```\n").size() == 1);
    TESSERA_CHECK(Check(dir, "<!-- source: src/a.txt -->\n```text\nfive\n...\none\n```\n").size() == 1);
}

TESSERA_TEST(guide_check_finds_a_block_without_a_marker) {
    TempDir dir;
    MakeRepository(dir);
    const std::vector<std::string> bare = Check(dir, "Some text.\n\n```cpp\nint x;\n```\n");
    TESSERA_CHECK(bare.size() == 1 && bare[0].rfind("[marker] line 3", 0) == 0);
    // A shell block needs its marker too, and with it is not compared.
    guide_check::Stats stats;
    TESSERA_CHECK(Check(dir, "<!-- shell -->\n```powershell\nanything at all\n```\n", &stats).empty());
    TESSERA_CHECK(stats.shells == 1);
    // A marker is for the block right after it, not for one after some text.
    TESSERA_CHECK(Count(Check(dir, "<!-- shell -->\nText in between.\n```bash\nls\n```\n"), "[marker]") == 1);
    // And it is for one block only.
    TESSERA_CHECK(Count(Check(dir, "<!-- shell -->\n```bash\nls\n```\n\n```bash\nls\n```\n"), "[marker]") ==
                  1);
    // A block that is never closed is a problem too.
    TESSERA_CHECK(Count(Check(dir, "<!-- shell -->\n```bash\nls\n"), "[marker]") == 1);
}

TESSERA_TEST(guide_check_finds_an_excerpt_of_a_missing_file) {
    TempDir dir;
    MakeRepository(dir);
    const std::vector<std::string> missing =
        Check(dir, "<!-- source: src/nothing.txt -->\n```text\none\n```\n");
    TESSERA_CHECK(missing.size() == 1);
    TESSERA_CHECK(Has(missing[0], "[source] line 2") && Has(missing[0], "src/nothing.txt does not exist"));
}

TESSERA_TEST(guide_check_finds_an_output_that_differs) {
    TempDir dir;
    MakeRepository(dir);
    // The right output, with the path of the repository left out.
    const std::string right = "<!-- output: check schema.json -->\n```text\nschema.json: OK, 2 types, hash "
                              "0x04b6ebf2e86202d0\n```\n";
    guide_check::Stats stats;
    TESSERA_CHECK(Check(dir, right, &stats).empty());
    TESSERA_CHECK(stats.outputs == 1);
    TESSERA_CHECK(
        Check(dir, "<!-- output: hash schema.json -->\n```text\n0x04b6ebf2e86202d0\n```\n").empty());
    // A different number, and a command that fails.
    const std::vector<std::string> other =
        Check(dir, "<!-- output: hash schema.json -->\n```text\n0x04b6ebf2e86202d1\n```\n");
    TESSERA_CHECK(other.size() == 1 && Has(other[0], "[output] line 2") &&
                  Has(other[0], "0x04b6ebf2e86202d0"));
    const std::vector<std::string> fails =
        Check(dir, "<!-- output: hash nothing.json -->\n```text\nx\n```\n");
    TESSERA_CHECK(fails.size() == 1 && Has(fails[0], "exits with 3"));
}

TESSERA_TEST(guide_check_finds_a_number_in_the_prose_and_a_broken_link) {
    TempDir dir;
    MakeRepository(dir);
    const std::string guide =
        "The hash is 1234567890.\n"
        "A short number like 12345 is fine, and so is a link to [the other page](other.md#top).\n"
        "A hexadecimal number such as 0xdeadbeef is not.\n"
        "A link to [nothing](missing.md) is broken, and [one on the web](https://example.org/x) is not "
        "looked at.\n"
        "<!-- source: src/a.txt -->\n```text\n9999999999\n```\n";
    const std::vector<std::string> problems = Check(dir, guide);
    TESSERA_CHECK(Count(problems, "[prose]") == 2);
    TESSERA_CHECK(Count(problems, "[link]") == 1);
    TESSERA_CHECK(Count(problems, "[source]") ==
                  1); // the number in the block is for the block to say, not the prose
    TESSERA_CHECK(Has(problems[0], "line 1") && Has(problems[1], "line 3"));
    TESSERA_CHECK(Has(problems[2], "line 4") && Has(problems[2], "missing.md"));
}

TESSERA_TEST(guide_check_refuses_an_output_that_would_write_a_file) {
    TempDir dir;
    MakeRepository(dir);
    const std::string target = dir.File("out.hpp");
    const std::vector<std::string> writes = Check(
        dir, "<!-- output: generate --cpp " + target + " schema.json -->\n```text\nanything\n```\n");
    TESSERA_CHECK(writes.size() == 1 && Has(writes[0], "[output] line 2") && Has(writes[0], "--check"));
    // The command was not run: nothing was written.
    TESSERA_CHECK(!std::filesystem::exists(target));
    // With --check it is run, and says that the file is not there.
    const std::vector<std::string> checks = Check(
        dir, "<!-- output: generate --check --cpp " + target + " schema.json -->\n```text\nanything\n```\n");
    TESSERA_CHECK(checks.size() == 1 && Has(checks[0], "exits with 1"));
    TESSERA_CHECK(!std::filesystem::exists(target));
}

TESSERA_TEST(guide_check_finds_code_blocks_that_are_made_in_other_ways) {
    TempDir dir;
    MakeRepository(dir);
    // A fence made of tildes, and a block indented by four spaces, need a marker like any other.
    TESSERA_CHECK(Count(Check(dir, "Text.\n\n~~~cpp\nint x;\n~~~\n"), "[marker]") == 1);
    TESSERA_CHECK(Count(Check(dir, "Text.\n\n    int x;\n    int y;\n"), "[marker]") == 1);
    TESSERA_CHECK(Count(Check(dir, "Text.\n\n\tint x;\n"), "[marker]") == 1);
    // With a marker the tilde fence is fine, and a nested item of a list is not a block of code.
    TESSERA_CHECK(Check(dir, "<!-- shell -->\n~~~bash\nls\n~~~\n").empty());
    TESSERA_CHECK(Check(dir, "- one\n\n    - nested\n\n    1. numbered\n").empty());
}
