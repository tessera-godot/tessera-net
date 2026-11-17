# SPDX-License-Identifier: Apache-2.0
"""Checks (or fixes) the formatting of the C++ files that git tracks, with one exact version of clang-format.

Usage:
    python tools/check_format.py [--fix] [--clang-format <path to clang-format>]

Without arguments it only checks: it lists the files that are not formatted and exits with 1. With --fix it
rewrites them. Only files tracked by git are looked at, so a file that is not added yet is not checked.

The clang-format is looked for in --clang-format, then in the CLANG_FORMAT environment variable, then in PATH. Its
version has to be the one in tools/requirements-format.txt, because other versions format a few things differently.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

# The version of clang-format the code is formatted with. It has to be the same as in tools/requirements-format.txt.
PINNED_VERSION = "18.1.8"

PATTERNS = ["*.cpp", "*.hpp", "*.h", "*.cc", "*.c"]
BATCH = 40  # files for each clang-format run, to keep the command line short on Windows
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REQUIREMENTS = os.path.join(ROOT, "tools", "requirements-format.txt")

# A line of clang-format output that is a problem other than "code should be clang-formatted".
OTHER_PROBLEM = re.compile(
    r"^(?:.+?:\d+:\d+: error: (?!code should be clang-formatted)|error:|Error |YAML:|.*[Nn]o such file)",
    re.MULTILINE,
)

SETUP_HELP ="""To get the right clang-format without installing anything on the system, make a virtual environment:
  python -m venv <folder>
  <folder>/bin/python -m pip install --require-hashes -r tools/requirements-format.txt   (Windows: <folder>\\Scripts\\python.exe)
then run this script again with CLANG_FORMAT=<folder>/bin/clang-format (Windows: <folder>\\Scripts\\clang-format.exe)
or with --clang-format <that path>."""


def pinned_in_requirements():
    """The version that tools/requirements-format.txt asks for, or None if the file has none."""
    try:
        with open(REQUIREMENTS, encoding="utf-8") as requirements:
            for line in requirements:
                match = re.match(r"\s*clang-format==([0-9][0-9.]*)", line)
                if match:
                    return match.group(1)
    except OSError:
        pass
    return None


def find_clang_format(explicit):
    """The path of the clang-format to use, or None."""
    candidate = explicit or os.environ.get("CLANG_FORMAT")
    if candidate:
        return candidate if os.path.isfile(candidate) else None
    return shutil.which("clang-format")


def version_of(executable):
    """The version a clang-format reports (for example "18.1.8"), or None if it cannot be run or read."""
    try:
        result = subprocess.run(
            [executable, "--version"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=60,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    match = re.search(r"clang-format version (\S+)", result.stdout)
    return match.group(1) if match else None


def tracked_files():
    """The C++ files git tracks, as paths relative to the repository root."""
    result = subprocess.run(
        ["git", "-C", ROOT, "ls-files", "-z", "--", *PATTERNS],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    if result.returncode != 0:
        print("git could not list the files:\n" + result.stderr)
        return None
    return [name for name in result.stdout.split("\0") if name]


def run_clang_format(executable, arguments, files):
    return subprocess.run(
        [executable, "--style=file", *arguments, *files],
        cwd=ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )


def check(executable, files):
    """Returns the files that are not formatted, or None if clang-format itself failed."""
    unformatted = []
    for start in range(0, len(files), BATCH):
        batch = files[start : start + BATCH]
        result = run_clang_format(executable, ["--dry-run", "--Werror"], batch)
        found = {
            match.group(1).replace("\\", "/")
            for match in re.finditer(r"^(.+?):\d+:\d+: (?:error|warning):", result.stderr, re.MULTILINE)
        }
        unformatted.extend(name for name in batch if name in found)
        if result.returncode != 0 and (not found or OTHER_PROBLEM.search(result.stderr)):
            # Not only files out of format: clang-format could not read a file or its settings. Stopping here keeps
            # that from hiding behind the list of files to format.
            print("clang-format failed:\n" + result.stderr)
            return None
    return unformatted


def fix(executable, files):
    for start in range(0, len(files), BATCH):
        result = run_clang_format(executable, ["-i"], files[start : start + BATCH])
        if result.returncode != 0:
            print("clang-format failed:\n" + result.stderr)
            return False
    return True


def main():
    parser = argparse.ArgumentParser(description="Check or fix the formatting of the tracked C++ files.")
    parser.add_argument("--fix", action="store_true", help="rewrite the files that are not formatted")
    parser.add_argument("--clang-format", dest="clang_format", help="path to clang-format %s" % PINNED_VERSION)
    arguments = parser.parse_args()

    if pinned_in_requirements() != PINNED_VERSION:
        print(
            f"tools/requirements-format.txt asks for clang-format {pinned_in_requirements()}, but this script is "
            f"set to {PINNED_VERSION}. Make them the same."
        )
        return 1

    executable = find_clang_format(arguments.clang_format)
    if executable is None:
        where = arguments.clang_format or os.environ.get("CLANG_FORMAT")
        print(("No clang-format at " + where + ".") if where else "clang-format was not found.")
        print(SETUP_HELP)
        return 1
    found_version = version_of(executable)
    if found_version != PINNED_VERSION:
        print(f"{executable} is version {found_version}, but {PINNED_VERSION} is needed.")
        print(SETUP_HELP)
        return 1

    files = tracked_files()
    if files is None:
        return 1
    if not files:
        print("git lists no C++ files, so there is nothing to check. Run this from a clone of the repository.")
        return 1

    if arguments.fix:
        if not fix(executable, files):
            return 1
        print(f"Formatted {len(files)} files with clang-format {PINNED_VERSION}.")
        return 0

    unformatted = check(executable, files)
    if unformatted is None:
        return 1
    if unformatted:
        print(f"{len(unformatted)} of {len(files)} files are not formatted with clang-format {PINNED_VERSION}:")
        for name in unformatted:
            print("  " + name)
        print("Run `python tools/check_format.py --fix` to format them.")
        return 1
    print(f"All {len(files)} files are formatted (clang-format {PINNED_VERSION}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
