"""Runs a headless Godot server plus K bot clients for each scenario and summarizes the metrics.

Usage: python run.py <godot executable> <entities,entities,...> <clients> <run_seconds> [moving_pct]
Requires: Godot 4.x, and `pip install psutil`. Use the console build of Godot on Windows.
"""
import json
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

import psutil

PROJECT = str(Path(__file__).resolve().parent)
STEADY_FROM = 8.0  # seconds after all clients connected


def reader(stream, sink, on_line=None):
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


def run_scenario(godot, entities, clients, run_s, moving, port):
    base = [godot, "--headless", "--path", PROJECT, "--"]
    server = subprocess.Popen(
        base + ["role=server", f"port={port}", f"entities={entities}", f"clients={clients}",
                f"run={run_s}", f"moving={moving}"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    sproc = psutil.Process(server.pid)
    ticks, cpu_marks = [], []

    def on_tick(obj):
        if obj.get("event") == "tick":
            total = 0.0
            for proc in [sproc] + sproc.children(recursive=True):  # console.exe is a launcher
                try:
                    ct = proc.cpu_times()
                    total += ct.user + ct.system
                except psutil.Error:
                    pass
            cpu_marks.append((time.time(), total))

    threading.Thread(target=reader, args=(server.stdout, ticks, on_tick), daemon=True).start()
    time.sleep(3 + entities / 2000)  # let the server spawn its entities
    procs, client_logs = [], []
    for _ in range(clients):
        p = subprocess.Popen(base + ["role=client", f"port={port}"], stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, text=True)
        log = []
        threading.Thread(target=reader, args=(p.stdout, log), daemon=True).start()
        procs.append(p)
        client_logs.append(log)
        time.sleep(0.15)
    try:
        server.wait(timeout=run_s + 150)
    except subprocess.TimeoutExpired:
        server.kill()
    for p in procs:
        p.kill()

    tk = [t for t in ticks if t.get("event") == "tick"]
    join = [t for t in tk if 0 <= t["since_all"] < STEADY_FROM]
    steady = [t for t in tk if t["since_all"] >= STEADY_FROM]
    result = {"entities": entities, "clients": clients, "moving_pct": moving}
    if not steady:
        result["error"] = "no steady-state samples (server did not finish or clients did not connect)"
        return result
    result["fps_avg"] = round(statistics.mean(t["fps"] for t in steady), 1)
    result["process_ms_avg"] = round(statistics.mean(t["process_ms"] for t in steady), 2)
    result["process_ms_max"] = round(max(t["process_ms"] for t in steady), 2)
    result["physics_ms_avg"] = round(statistics.mean(t["physics_ms"] for t in steady), 2)
    sent = statistics.mean(t["sent_bytes"] for t in steady)
    result["sent_KBps_total"] = round(sent / 1024, 1)
    result["sent_KBps_per_client"] = round(sent / 1024 / clients, 2)
    result["bytes_per_entity_per_client_per_s"] = round(sent / clients / entities, 2)
    result["join_burst_MB_total"] = round(sum(t["sent_bytes"] for t in join) / 1e6, 2)
    # server CPU over the steady window (one core = 100%)
    marks = [m for m, t in zip(cpu_marks, tk) if t["since_all"] >= STEADY_FROM]
    if len(marks) >= 2:
        result["server_cpu_pct_of_one_core"] = round(100 * (marks[-1][1] - marks[0][1]) / (marks[-1][0] - marks[0][0]), 1)
    seen = [log[-1]["entities"] for log in client_logs if log]
    result["clients_saw_min_entities"] = min(seen) if seen else 0
    return result


def main():
    godot = sys.argv[1]
    entity_list = [int(x) for x in sys.argv[2].split(",")]
    clients = int(sys.argv[3])
    run_s = int(sys.argv[4])
    moving = int(sys.argv[5]) if len(sys.argv) > 5 else 100
    results = []
    for i, n in enumerate(entity_list):
        print(f"--- scenario entities={n} clients={clients} moving={moving}%", flush=True)
        r = run_scenario(godot, n, clients, run_s, moving, 9100 + i)
        print(json.dumps(r), flush=True)
        results.append(r)
    out = Path(PROJECT) / f"results_{int(time.time())}.json"
    out.write_text(json.dumps(results, indent=2))
    print("saved", out.name)


if __name__ == "__main__":
    main()
