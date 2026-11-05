# Godot built-in replication baseline

Measures how a headless Godot server behaves when it replicates many entities with the built-in
`MultiplayerSynchronizer`. The results are on the [benchmarks page](../../docs/benchmarks.md).

Each entity is a node with a synchronizer that replicates its position at 20 Hz. The server moves every
entity, and bot clients (separate Godot processes) connect and see every entity.

```
pip install psutil
python run.py <path to the Godot console executable> 500,2000,5000,10000 8 25
```

Arguments: the Godot executable, a comma-separated list of entity counts, the number of bot clients, and
how many seconds to measure after all clients are connected. The script prints one line of results per
scenario (server frame rate, CPU, bandwidth) and saves them to a JSON file.

Notes: everything runs on one machine over localhost, so there is no latency or packet loss; only positions
are replicated; the first column of "bytes per entity" includes protocol overhead. Treat the numbers as an
order of magnitude.
