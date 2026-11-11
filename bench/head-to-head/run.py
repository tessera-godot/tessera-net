"""Runs the Tessera Net head-to-head server plus K bot clients for each scenario and summarizes the metrics, in
the form of bench/godot-replication/run.py. With --godot it runs that baseline too, alternating the two in one
session, and prints the results side by side.

Usage: python run.py <tessera_h2h_server.exe> <entities,entities,...> <clients> <run_seconds> [moving_pct]
                     [--threads 1] [--relevancy all|spatial] [--rounds 1] [--godot <godot console exe>]
                     [--silence <tessera_net_bench.exe>] [--out <file>] [--timeout 150]
Requires: the ENet build of the programs (-DTESSERA_BUILD_ENET=ON) and `pip install psutil`.
The bot is the program next to the server, tessera_h2h_bot.exe.
"""
import argparse
import importlib.util
import json
import re
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

import psutil

HERE = Path(__file__).resolve().parent
GODOT_RUN = HERE.parent / "godot-replication" / "run.py"
STEADY_FROM = 8.0  # seconds after all clients connected, as in the baseline
BASE_PORT = 9300   # the Godot baseline uses 9100 and up
TICK_HZ = 20


def reader(stream, sink, on_line=None):
    """Collects the JSON lines a program prints; `on_line` sees each one first and may annotate it."""
    for raw in stream:
        line = raw.strip()
        if line.startswith("{"):
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            if on_line:
                on_line(obj)
            sink.append(obj)


def send_line(proc, text):
    try:
        proc.stdin.write(text + "\n")
        proc.stdin.flush()
    except (OSError, ValueError):
        pass


def stop(proc, grace=5.0):
    """Ends a process this script started: asks it to quit, then kills it if it is still there."""
    send_line(proc, "quit")
    try:
        proc.wait(timeout=grace)
    except subprocess.TimeoutExpired:
        proc.kill()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass


def run_scenario(server_exe, entities, clients, run_s, moving, port, threads=1, relevancy="all", timeout=150):
    bot_exe = str(Path(server_exe).with_name("tessera_h2h_bot.exe"))
    server = subprocess.Popen(
        [server_exe, f"port={port}", f"entities={entities}", f"clients={clients}", f"run={run_s}",
         f"moving={moving}", f"threads={threads}", f"relevancy={relevancy}"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    sproc = psutil.Process(server.pid)
    events = []
    spawned = threading.Event()
    settled = threading.Event()
    procs = [server]
    bot_logs = []

    def on_event(obj):
        obj["_rx"] = time.time()
        kind = obj.get("event")
        if kind == "tick":
            try:
                ct = sproc.cpu_times()
                obj["_cpu"] = ct.user + ct.system
            except psutil.Error:
                pass
        elif kind == "spawned":
            spawned.set()
        elif kind == "settled":
            settled.set()

    result = {"entities": entities, "clients": clients, "moving_pct": moving}
    try:
        threading.Thread(target=reader, args=(server.stdout, events, on_event), daemon=True).start()
        if not spawned.wait(timeout=60 + entities / 100):
            result["error"] = "the server did not start"
            return result
        spawn = next(e for e in events if e.get("event") == "spawned")
        for _ in range(clients):
            p = subprocess.Popen([bot_exe, f"port={port}", f"epoch_ms={spawn['epoch_ms']}"],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                 text=True)
            log = []
            threading.Thread(target=reader, args=(p.stdout, log, lambda o: o.update(_rx=time.time())),
                             daemon=True).start()
            procs.append(p)
            bot_logs.append(log)
            time.sleep(0.15)

        deadline = time.time() + run_s + timeout
        while not settled.is_set() and server.poll() is None and time.time() < deadline:
            settled.wait(timeout=0.5)
        if not settled.is_set():
            result["error"] = "the state did not settle (the server did not finish or clients did not connect)"
            return result
        done = next(e for e in events if e.get("event") == "settled")
        time.sleep(1.0)  # the bots take their last updates

        # Convergence (R3): every bot reports what it holds, and it is compared with the server's checksum.
        for p in procs[1:]:
            send_line(p, "checksum")
        reports = []
        report_deadline = time.time() + 10
        for log in bot_logs:
            while time.time() < report_deadline and not any(e.get("event") == "checksum" for e in log):
                time.sleep(0.05)
            reports.append(next((e for e in log if e.get("event") == "checksum"), None))
        visible = {v["connection"]: v["count"] for v in done.get("visible", [])}
        converged = True
        for i, report in enumerate(reports):
            if report is None:
                print(f"! bot {i} did not answer the checksum request", flush=True)
                converged = False
            elif relevancy == "spatial":
                expected = visible.get(report["connection"])
                if expected is None or report["entities"] != expected:
                    print(f"! bot {i} diverged: sees {report['entities']} entities, the server says "
                          f"{expected}", flush=True)
                    converged = False
            elif report["entities"] != entities or report["checksum"] != done["checksum"]:
                print(f"! bot {i} diverged: {report['entities']} entities, checksum {report['checksum']} "
                      f"(server: {entities} entities, {done['checksum']})", flush=True)
                converged = False
    finally:
        for p in procs[1:]:
            stop(p)
        stop(server)

    tk = [e for e in events if e.get("event") == "tick"]
    join = [t for t in tk if 0 <= t["since_all"] < STEADY_FROM]
    steady = [t for t in tk if STEADY_FROM <= t["since_all"] <= run_s]  # while the entities still move
    if not steady:
        result["error"] = "no steady-state samples (the run is too short or the clients did not connect)"
        return result
    result["fps_avg"] = round(statistics.mean(t["fps"] for t in steady), 1)
    result["process_ms_avg"] = round(statistics.mean(t["process_ms"] for t in steady), 2)
    result["process_ms_max"] = round(max(t["process_ms_max"] for t in steady), 2)
    # The time the server spent working, from its own clock (one core = 100%). The process times that psutil
    # reads are charged in scheduler quanta, so a server that works a millisecond or two per tick can show
    # less than it used; this figure is there to check it.
    result["server_busy_pct_of_one_core"] = round(
        statistics.mean(t["process_ms"] * t["fps"] / 10 for t in steady), 1)
    result["physics_ms_avg"] = round(statistics.mean(t["physics_ms"] for t in steady), 2)
    result["update_ms_avg"] = round(statistics.mean(t["update_ms"] for t in steady), 2)
    result["transport_ms_avg"] = round(statistics.mean(t["transport_ms"] for t in steady), 2)
    sent = statistics.mean(t["sent_bytes"] for t in steady)
    # With the relevancy on, a client does not see every entity: per-entity figures use what it sees.
    per_client = statistics.mean(visible.values()) if relevancy == "spatial" and visible else entities
    result["sent_KBps_total"] = round(sent / 1024, 1)
    result["sent_KBps_per_client"] = round(sent / 1024 / clients, 2)
    result["bytes_per_entity_per_client_per_s"] = round(sent / clients / per_client, 2)
    result["bytes_per_update"] = round(sent / clients / per_client / TICK_HZ, 2)
    result["join_burst_MB_total"] = round(sum(t["sent_bytes"] for t in join) / 1e6, 2)
    marks = [t for t in steady if "_cpu" in t]
    if len(marks) >= 2:
        result["server_cpu_pct_of_one_core"] = round(
            100 * (marks[-1]["_cpu"] - marks[0]["_cpu"]) / (marks[-1]["_rx"] - marks[0]["_rx"]), 1)
    seen = [[e for e in log if e.get("event") == "client"] for log in bot_logs]
    result["clients_saw_min_entities"] = min((s[-1]["entities"] for s in seen if s), default=0)
    lags = [e["lag_ms"] for s in seen for e in s
            if steady[0]["_rx"] <= e["_rx"] <= done["_rx"] and e["latest_tick"] > 0]
    result["lag_ms_avg"] = round(statistics.mean(lags), 1) if lags else None
    result["converged"] = converged
    result["checksum_server"] = done["checksum"]
    if relevancy == "spatial":
        result["visible_mean"] = round(per_client, 1)
    return result


def load_godot_runner():
    spec = importlib.util.spec_from_file_location("godot_replication_run", GODOT_RUN)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.run_scenario


class ChildWatcher:
    """Remembers every process this script starts (the children and their children) while the Godot baseline
    runs, so that afterwards it can end the ones that are still there and nothing else. The Godot console
    program is a launcher, and its child can outlive it."""

    def __init__(self):
        self.seen = {}  # pid -> create_time, so that a reused pid is not mistaken for ours
        self.running = True
        self.thread = threading.Thread(target=self._watch, daemon=True)
        self.thread.start()

    def _watch(self):
        me = psutil.Process()
        while self.running:
            try:
                for child in me.children(recursive=True):
                    try:
                        self.seen[child.pid] = child.create_time()
                    except psutil.Error:
                        pass
            except psutil.Error:
                pass
            time.sleep(0.25)

    def finish(self):
        self.running = False
        self.thread.join(timeout=2)
        for pid, created in self.seen.items():
            try:
                proc = psutil.Process(pid)
                if proc.create_time() == created:
                    proc.kill()
            except psutil.Error:
                pass


def write_record_indicator(bench_exe):
    """The time of one WriteRecord, in ns, which shows how busy the machine is (quiet is about 47)."""
    out = subprocess.run([bench_exe, "indicator"], capture_output=True, text=True, timeout=60).stdout
    found = re.search(r"WriteRecord indicator\s+([0-9.]+)", out)
    return float(found.group(1)) if found else None


def machine_load():
    """Total CPU use over one second and the names of the eight busiest programs."""
    for proc in psutil.process_iter():
        try:
            proc.cpu_percent(None)
        except psutil.Error:
            pass
    total = psutil.cpu_percent(interval=1)
    busy = []
    for proc in psutil.process_iter(["name"]):
        try:
            busy.append((proc.cpu_percent(None), proc.info["name"]))
        except psutil.Error:
            pass
    busy.sort(reverse=True)
    return {"cpu_pct": total, "busiest": [name for _, name in busy[:8]]}


def median_range(values):
    values = [v for v in values if v is not None]
    if not values:
        return None
    return statistics.median(values), min(values), max(values)


def cell(stats, digits=1):
    if stats is None:
        return "-"
    med, low, high = stats
    return f"{med:.{digits}f} [{low:.{digits}f}-{high:.{digits}f}]"


def print_table(runs, sizes):
    print("\n=== side by side: median [min-max] over the rounds ===", flush=True)
    print(f"{'entities':>8}  {'metric':<28} {'godot':>26} {'tessera':>26} {'gain':>8}", flush=True)
    for n in sizes:
        godot = [r["result"] for r in runs if r["system"] == "godot" and r["entities"] == n
                 and "error" not in r["result"]]
        ours = [r["result"] for r in runs if r["system"] == "tessera" and r["entities"] == n
                and "error" not in r["result"]]
        rows = [
            ("server_cpu_pct_of_one_core", lambda r: r.get("server_cpu_pct_of_one_core"), 1, True),
            ("server_busy_pct_of_one_core", lambda r: r.get("server_busy_pct_of_one_core"), 1, False),
            ("fps_avg", lambda r: r.get("fps_avg"), 1, False),
            ("process_ms_avg", lambda r: r.get("process_ms_avg"), 2, False),
            ("sent_KBps_per_client", lambda r: r.get("sent_KBps_per_client"), 2, True),
            ("bytes_per_update", lambda r: r.get("bytes_per_update", r.get("bytes_per_entity_per_client_per_s", 0)
                                                  / TICK_HZ), 2, False),
        ]
        for name, get, digits, with_gain in rows:
            g = median_range([get(r) for r in godot])
            t = median_range([get(r) for r in ours])
            gain = f"{g[0] / t[0]:.1f}x" if with_gain and g and t and t[0] else ""
            print(f"{n:>8}  {name:<28} {cell(g, digits):>26} {cell(t, digits):>26} {gain:>8}", flush=True)
        failed = [r for r in runs if r["entities"] == n and "error" in r["result"]]
        if failed:
            print(f"{n:>8}  runs with an error: " + ", ".join(f"{r['system']} round {r['round']}" for r in failed),
                  flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("server_exe")
    parser.add_argument("entities", help="comma-separated entity counts")
    parser.add_argument("clients", type=int)
    parser.add_argument("run_seconds", type=int)
    parser.add_argument("moving_pct", type=int, nargs="?", default=100)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--relevancy", choices=["all", "spatial"], default="all")
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--godot", help="the Godot console executable: also runs the baseline, alternating")
    parser.add_argument("--silence", help="tessera_net_bench.exe: records the WriteRecord indicator")
    parser.add_argument("--out", help="a file for the results, as JSON")
    parser.add_argument("--timeout", type=int, default=150, help="seconds to wait for a run beyond its length")
    args = parser.parse_args()
    if args.godot and args.relevancy != "all":
        parser.error("--godot compares the main scenario: use --relevancy all")

    sizes = [int(x) for x in args.entities.split(",")]
    godot_run = load_godot_runner() if args.godot else None
    runs, silence = [], {}
    index = 0
    for n in sizes:
        if args.silence:
            silence[n] = {"before": write_record_indicator(args.silence)}
        for rnd in range(1, args.rounds + 1):
            systems = ["tessera", "godot"] if args.godot else ["tessera"]
            if rnd % 2 == 0:
                systems.reverse()  # who goes first changes every round
            for system in systems:
                label = f" system={system} round={rnd}" if (args.godot or args.rounds > 1) else ""
                print(f"--- scenario entities={n} clients={args.clients} moving={args.moving_pct}%{label}",
                      flush=True)
                load = machine_load() if args.silence else None
                port = BASE_PORT + index if system == "tessera" else 9100 + index
                index += 1
                started = time.time()
                watcher = ChildWatcher() if system == "godot" else None
                try:
                    if system == "tessera":
                        result = run_scenario(args.server_exe, n, args.clients, args.run_seconds,
                                              args.moving_pct, port, args.threads, args.relevancy, args.timeout)
                    else:
                        result = godot_run(args.godot, n, args.clients, args.run_seconds, args.moving_pct, port)
                except Exception as error:  # a failed run is a row of the table, not the end of the session
                    result = {"entities": n, "clients": args.clients, "error": repr(error)}
                finally:
                    if watcher:
                        watcher.finish()
                print(json.dumps(result), flush=True)
                runs.append({"system": system, "entities": n, "round": rnd, "seconds": round(time.time() - started, 1),
                             "load": load, "result": result})
        if args.silence:
            silence[n]["after"] = write_record_indicator(args.silence)
            print(f"WriteRecord indicator for {n}: {silence[n]['before']} -> {silence[n]['after']} ns", flush=True)

    if args.godot:
        print_table(runs, sizes)
    if args.out:
        Path(args.out).write_text(json.dumps({"runs": runs, "silence": silence, "relevancy": args.relevancy,
                                              "threads": args.threads}, indent=2))
        print("saved", args.out, flush=True)
    failed = [r for r in runs if r["system"] == "tessera" and not r["result"].get("converged", False)]
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
