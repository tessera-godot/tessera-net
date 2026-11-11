# Head to head with Godot's built-in replication

Two programs and a script that run the scenario of [`../godot-replication/`](../godot-replication/) on a Tessera
Net server, over real sockets (ENet), so that the two can be measured one after the other on the same machine.

- `tessera_h2h_server`: a `ReplicationServer` over ENet with N entities in a square world of 2 km by 2 km, all
  of them moving at up to 5 m/s along each ground axis, replicated at 20 Hz. Only the position is replicated,
  every entity is visible to every client, and there is no byte budget. It prints one JSON line per second
  (tick time, bytes sent and received) and, once the entities have stopped, the checksum of their state.
- `tessera_h2h_bot`: a `ClientReplica` over ENet that only receives. It prints how many entities it sees and how
  late the state is, and answers a `checksum` line on standard input with the checksum of what it holds.
- `run.py`: starts the server and K bots as separate processes, reads the lines, measures the server's CPU
  (user plus system time, one core is 100%), bandwidth per client and bytes per update, and checks that every
  bot ends up holding exactly the server's state. It takes the arguments of the Godot script and prints the same
  lines.

Only Windows has been measured.

## Build

The programs need ENet, which is downloaded on the first configure and only when you ask for it:

```powershell
cmake --preset windows-msvc-enet
cmake --build --preset windows-msvc-enet-release --target tessera_h2h_server tessera_h2h_bot
```

## Run

```powershell
python bench/head-to-head/run.py build/windows-msvc-enet/bench/head-to-head/Release/tessera_h2h_server.exe 500,2000 8 25
```

The arguments are the server program, the numbers of entities, the number of bots, the seconds to run once
every bot has connected and, optionally, the percentage of entities that move (100 by default). The script needs
`psutil`. Options:

| Option | Meaning |
|---|---|
| `--threads N` | Workers of the server, the calling thread included (1 by default, the figure "per core") |
| `--relevancy spatial` | Turns the spatial relevancy on (each bot sees what is within `range`, 250 m by default); the Godot baseline has no counterpart |
| `--rounds N` | Runs each size N times |
| `--godot <console exe>` | Also runs the Godot baseline, alternating the two systems, and prints the results side by side |
| `--silence <tessera_net_bench.exe>` | Records the `WriteRecord` indicator before and after each size, and the load of the machine before each run |
| `--out <file>` | Writes every result as JSON |
| `--timeout S` | Seconds to wait for a run beyond its length (150 by default) |

Each result is one JSON line with the keys of the Godot script plus `update_ms_avg`, `transport_ms_avg` (the
part of the tick spent in the transport), `bytes_per_update`, `lag_ms_avg`, `converged` and `checksum_server`.
`update_ms_avg` includes `transport_ms_avg`. Bandwidth is what ENet put on the sockets, the quantity the Godot
script reads from its own ENet host. `converged` is true when every bot holds the same state as the server;
the script exits with 1 if any run of ours did not converge.

The programs take `key=value` arguments too (`port`, `entities`, `clients`, `run`, `moving`, `threads`,
`relevancy`, `range`) if you want to start them by hand; the bot takes `port`, `host` and `epoch_ms`.
