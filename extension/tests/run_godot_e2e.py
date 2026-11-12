# SPDX-License-Identifier: Apache-2.0
"""Runs the extension inside a headless Godot.

Usage:
    python -I run_godot_e2e.py load <godot> <stage_dir>

The stage directory is the one the build assembles (the test project, the .gdextension file and the library).
The script only ever ends a process it started itself, and only when that process has passed its time limit.
"""
import os
import subprocess
import sys

IMPORT_LIMIT = 60.0  # seconds
RUN_LIMIT = 60.0


def run(command, limit):
    """Runs a command, collects its output, and returns (exit code, output). A command that passes the limit
    is ended (it is our own child) and reported with the code None."""
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        output, _ = process.communicate(timeout=limit)
        return process.returncode, output
    except subprocess.TimeoutExpired:
        process.kill()
        output, _ = process.communicate()
        return None, output


def import_project(godot, stage):
    """Lets Godot read the project once, so that it knows about the extension."""
    code, output = run([godot, "--headless", "--path", stage, "--import"], IMPORT_LIMIT)
    if code is None:
        print("FAIL import: passed the time limit\n" + output)
        return False
    return True


def load(godot, stage):
    if not import_project(godot, stage):
        return 1
    code, output = run([godot, "--headless", "--path", stage, "-s", "res://load_check.gd"], RUN_LIMIT)
    print(output)
    if code is None:
        print("FAIL load: passed the time limit")
        return 1
    if code != 0 or "LOAD ok" not in output:
        print(f"FAIL load: exit code {code}")
        return 1
    print("LOAD PASS")
    return 0


def main():
    if len(sys.argv) != 4 or sys.argv[1] != "load":
        print(__doc__)
        return 2
    _, mode, godot, stage = sys.argv
    if not os.path.isdir(stage):
        print(f"FAIL: the stage directory does not exist: {stage}")
        return 1
    if not os.path.isfile(godot):
        print(f"FAIL: the Godot executable does not exist: {godot}")
        return 1
    return load(godot, stage)


if __name__ == "__main__":
    sys.exit(main())
