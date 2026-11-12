# SPDX-License-Identifier: Apache-2.0
"""Starts the example server as a real process and checks how it starts, how it stops when it gets a
signal, and how it fails.

Usage: python -I run_example_process.py <path to tessera_example_server>

It exits with 0 only if every case passes; each failure prints the case and what went wrong. The only
processes it ends are the ones it started itself, and only when a case has failed to stop them.
"""
import os
import signal
import socket
import subprocess
import sys
import threading
import time

LIMIT = 10.0  # seconds a step may take; a step that takes longer fails the case
WINDOWS = os.name == "nt"


def free_port():
    """A UDP port nobody is using right now."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class Process:
    """One run of the server, with its output collected line by line."""

    def __init__(self, exe, *args):
        flags = subprocess.CREATE_NEW_PROCESS_GROUP if WINDOWS else 0
        self.proc = subprocess.Popen(
            [exe, *args],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            creationflags=flags,
        )
        self.out = []
        self.err = []
        self.signal_error = None
        self._threads = [
            threading.Thread(target=self._pump, args=(self.proc.stdout, self.out), daemon=True),
            threading.Thread(target=self._pump, args=(self.proc.stderr, self.err), daemon=True),
        ]
        for thread in self._threads:
            thread.start()

    @staticmethod
    def _pump(stream, lines):
        for line in stream:
            lines.append(line.rstrip("\r\n"))

    def wait_for_line(self, text):
        """Waits until a line of output contains `text`; False if the process ends or the limit passes."""
        deadline = time.monotonic() + LIMIT
        while time.monotonic() < deadline:
            if any(text in line for line in list(self.out)):
                return True
            if self.proc.poll() is not None:
                time.sleep(0.2)  # let the reader see the last lines
                return any(text in line for line in list(self.out))
            time.sleep(0.02)
        return False

    def signal(self, name):
        """Sends the signal the platform has for 'stop'. A process that is already gone is not an error; a
        signal that could not be sent to a process that is still running is, and is kept in `signal_error`."""
        try:
            if WINDOWS:
                self.proc.send_signal(signal.CTRL_BREAK_EVENT)
            else:
                self.proc.send_signal(getattr(signal, name))
        except OSError as error:
            if self.proc.poll() is None:
                self.signal_error = str(error)

    def wait_for_exit(self):
        """The exit code, or None if the process is still running after the limit."""
        try:
            code = self.proc.wait(timeout=LIMIT)
        except subprocess.TimeoutExpired:
            return None
        for thread in self._threads:
            thread.join(timeout=2.0)
        return code

    def output(self):
        return "\n".join(self.out)

    def errors(self):
        return "\n".join(self.err)

    def clean_up(self):
        """Ends the process, if a failed case left it running. It is a child of this script."""
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()


class Failure(Exception):
    pass


def expect(condition, message):
    if not condition:
        raise Failure(message)


def stops_on_signal(exe, name, twice=False):
    port = free_port()
    server = Process(exe, f"--port={port}", "--tick_rate=60")
    try:
        expect(server.wait_for_line("listening on port"), "no 'listening on port' line\n" + server.output())
        # Two signals back to back: the second one may arrive while the first is being handled, while the server
        # shuts down, or after it has left. All of those must end the same way.
        for _ in range(2 if twice else 1):
            server.signal(name)
            expect(
                server.signal_error is None,
                f"could not send the signal (no console?): {server.signal_error}",
            )
        code = server.wait_for_exit()
        expect(code is not None, f"still running {LIMIT:.0f} s after the signal")
        expect(code == 0, f"exit code {code}, expected 0\n{server.output()}\n{server.errors()}")
        stopped = [line for line in server.out if line.rstrip().endswith("stopped")]
        expect(any("INFO  stopped" in line for line in stopped), "no 'stopped' line\n" + server.output())
        expect(
            sum("INFO  stopped" in line for line in stopped) == 1,
            "'stopped' was written more than once\n" + server.output(),
        )
    finally:
        server.clean_up()


def case_sigterm(exe):
    stops_on_signal(exe, "SIGTERM")  # on Windows this sends Ctrl+Break


def case_sigint(exe):
    stops_on_signal(exe, "SIGINT")


def case_second_signal_back_to_back(exe):
    stops_on_signal(exe, "SIGTERM", twice=True)


def case_bad_config(exe):
    server = Process(exe, "--port=0")
    try:
        code = server.wait_for_exit()
        expect(code is not None, "did not exit")
        expect(code == 2, f"exit code {code}, expected 2")
        expect("port" in server.errors(), "the message does not name the key: " + server.errors())
        expect("error:" in server.errors(), "no 'error:' in the message: " + server.errors())
    finally:
        server.clean_up()


def case_port_in_use(exe):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as taken:
        if WINDOWS:
            # Without this, Windows lets a second socket share the port.
            taken.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        taken.bind(("0.0.0.0", 0))
        port = taken.getsockname()[1]
        server = Process(exe, f"--port={port}")
        try:
            code = server.wait_for_exit()
            expect(code is not None, "did not exit")
            expect(code == 1, f"exit code {code}, expected 1\n{server.output()}\n{server.errors()}")
            expect("error:" in server.errors(), "no 'error:' in the message: " + server.errors())
            expect("Traceback" not in server.errors(), "a stack trace instead of a message")
        finally:
            server.clean_up()


def case_help(exe):
    server = Process(exe, "--help")
    try:
        code = server.wait_for_exit()
        expect(code == 0, f"exit code {code}, expected 0")
        expect(
            "tick_rate" in server.output() and "port" in server.output(),
            "the help does not list the keys",
        )
    finally:
        server.clean_up()


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    exe = sys.argv[1]
    cases = [
        ("sigterm", case_sigterm),
        ("second_signal_back_to_back", case_second_signal_back_to_back),
        ("bad_config", case_bad_config),
        ("port_in_use", case_port_in_use),
        ("help", case_help),
    ]
    if not WINDOWS:
        cases.insert(1, ("sigint", case_sigint))  # Windows has no SIGINT for another process
    failed = 0
    for name, case in cases:
        try:
            case(exe)
            print(f"ok {name}", flush=True)
        except Failure as error:
            failed += 1
            print(f"FAILED {name}: {error}", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
