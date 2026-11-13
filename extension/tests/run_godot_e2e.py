# SPDX-License-Identifier: Apache-2.0
"""Runs the extension inside a headless Godot.

Usage:
    python -I run_godot_e2e.py load <godot> <stage_dir>
    python -I run_godot_e2e.py e2e  <godot> <stage_dir> <example_server>

The stage directory is the one the build assembles (the test project, the .gdextension file and the library).
`load` only checks that the extension loads. `e2e` also starts the example server, runs the test script against it
and stops the server again; the whole run has to fit in 60 seconds.

The script only ever ends a process it started itself, and only when that process has passed its time limit.
"""
import os
import signal
import socket
import subprocess
import sys
import threading
import time

IMPORT_LIMIT = 60.0  # seconds
RUN_LIMIT = 60.0
SERVER_START_LIMIT = 10.0
SERVER_STOP_LIMIT = 10.0
WINDOWS = os.name == "nt"


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


def run_watching(command, limit, on_line):
    """Like run, but the output is read while the command runs and every line goes to `on_line` (used to stop the
    second server when the script says so)."""
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    lines = []

    def pump():
        for line in process.stdout:
            lines.append(line.rstrip("\r\n"))

    reader = threading.Thread(target=pump, daemon=True)
    reader.start()
    deadline = time.monotonic() + limit
    handled = 0
    code = None
    while time.monotonic() < deadline:
        while handled < len(lines):
            on_line(lines[handled])
            handled += 1
        code = process.poll()
        if code is not None:
            break
        time.sleep(0.02)
    if code is None:
        process.kill()  # our own child, past its time limit
        process.wait()
    reader.join(timeout=5.0)
    while handled < len(lines):
        on_line(lines[handled])
        handled += 1
    return code, "\n".join(lines)


# Exit codes of a process that crashed: a negative signal number on Linux, the access violation status on Windows.
CRASH_CODES = {-6, -11, 134, 139, 0xC0000005, -1073741819}


def import_project(godot, stage):
    """Lets Godot read the project once, so that it knows about the extension."""
    # Godot 4.7.2 crashes at the end of the first import of a project that holds a GDExtension with a registered
    # class (any extension, not only ours): at exit it runs a deferred call that needs the editor's documentation
    # data, which has already been freed (EditorHelp::_gen_extensions_docs, with a null DocTools). The import
    # itself has finished by then. The second import exits with 0. So a crash is repeated once, and any other
    # failure (or a second crash) fails the test right away.
    for attempt in range(2):
        code, output = run([godot, "--headless", "--path", stage, "--import"], IMPORT_LIMIT)
        if code is None:
            print("FAIL import: passed the time limit\n" + output)
            return False
        if code == 0:
            return True
        if attempt == 1 or code not in CRASH_CODES:
            break
    print(f"FAIL import: Godot exited with code {code}\n" + output)
    return False


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


def free_port():
    """A UDP port nobody is using right now."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class Server:
    """The example server as a process, with its output collected line by line."""

    def __init__(self, exe, port, extra_args=()):
        flags = subprocess.CREATE_NEW_PROCESS_GROUP if WINDOWS else 0
        self.proc = subprocess.Popen(
            [exe, f"--port={port}", "--tick_rate=30", "--log_level=info", *extra_args],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            creationflags=flags,
        )
        self.lines = []
        self._reader = threading.Thread(target=self._pump, daemon=True)
        self._reader.start()

    def _pump(self):
        for line in self.proc.stdout:
            self.lines.append(line.rstrip("\r\n"))

    def wait_until_listening(self):
        deadline = time.monotonic() + SERVER_START_LIMIT
        while time.monotonic() < deadline:
            if any("listening on port" in line for line in list(self.lines)):
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.02)
        return False

    def stop(self):
        """Asks the server to stop the way an operator would, waits for it, and returns its exit code, or None if it
        did not stop in time (then it is ended: it is our own child)."""
        if self.proc.poll() is None:
            try:
                self.proc.send_signal(signal.CTRL_BREAK_EVENT if WINDOWS else signal.SIGINT)
            except OSError:
                pass
        try:
            code = self.proc.wait(timeout=SERVER_STOP_LIMIT)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            code = None
        self._reader.join(timeout=2.0)
        return code


def e2e(godot, stage, server_exe):
    started = time.monotonic()
    port = free_port()
    dead_port = free_port()
    while dead_port == port:
        dead_port = free_port()
    doomed_port = free_port()
    while doomed_port in (port, dead_port):
        doomed_port = free_port()
    input_port = free_port()
    while input_port in (port, dead_port, doomed_port):
        input_port = free_port()
    server = Server(server_exe, port)
    doomed = Server(server_exe, doomed_port)  # a second server, stopped while the test runs
    players = Server(server_exe, input_port, ["--players"])  # a third one, with an entity for each player
    doomed_stopped = []
    result = 1
    try:
        if not server.wait_until_listening() or not doomed.wait_until_listening() or not players.wait_until_listening():
            print("FAIL server: it did not start\n" + "\n".join(server.lines + doomed.lines + players.lines))
            return 1
        if not import_project(godot, stage):
            return 1
        remaining = max(5.0, RUN_LIMIT - (time.monotonic() - started))
        def on_line(line):
            if line.strip() == "STOP_SERVER" and not doomed_stopped:
                doomed_stopped.append(doomed.stop())

        code, output = run_watching(
            [
                godot,
                "--headless",
                "--path",
                stage,
                "-s",
                "res://e2e.gd",
                "--",
                f"--port={port}",
                f"--dead_port={dead_port}",
                f"--doomed_port={doomed_port}",
                f"--input_port={input_port}",
            ],
            remaining,
            on_line,
        )
        print(output)
        if code is None:
            print("FAIL e2e: passed the time limit")
        elif code != 0 or "E2E PASS" not in output:
            print(f"FAIL e2e: exit code {code}")
        else:
            result = 0
    finally:
        server_code = server.stop()
        if server_code != 0:
            print(f"FAIL server: exit code {server_code}, expected 0\n" + "\n".join(server.lines[-10:]))
            result = 1
        if not doomed_stopped:
            doomed_stopped.append(doomed.stop())
        if doomed_stopped[0] != 0:
            print(f"FAIL second server: exit code {doomed_stopped[0]}, expected 0\n" + "\n".join(doomed.lines[-10:]))
            result = 1
        players_code = players.stop()
        if players_code != 0:
            print(f"FAIL third server: exit code {players_code}, expected 0\n" + "\n".join(players.lines[-10:]))
            result = 1
    if result == 0:
        print(f"E2E RUN PASS in {time.monotonic() - started:.1f} s")
    return result


def main():
    modes = {"load": 4, "e2e": 5}
    if len(sys.argv) < 2 or sys.argv[1] not in modes or len(sys.argv) != modes[sys.argv[1]]:
        print(__doc__)
        return 2
    mode = sys.argv[1]
    # Absolute, native paths: a relative path with slashes is not found by Windows when starting a program.
    godot, stage = (os.path.abspath(path) for path in sys.argv[2:4])
    if not os.path.isdir(stage):
        print(f"FAIL: the stage directory does not exist: {stage}")
        return 1
    if not os.path.isfile(godot):
        print(f"FAIL: the Godot executable does not exist: {godot}")
        return 1
    if mode == "load":
        return load(godot, stage)
    server = os.path.abspath(sys.argv[4])
    if not os.path.isfile(server):
        print(f"FAIL: the example server does not exist: {server}")
        return 1
    return e2e(godot, stage, server)


if __name__ == "__main__":
    sys.exit(main())
