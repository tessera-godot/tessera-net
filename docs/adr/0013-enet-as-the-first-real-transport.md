# 0013. ENet as the first real transport

- **Status:** Proposed
- **Date:** 2026-11-11

## Context
Until now the server and the client have only run over `SimulatedNetwork`, a network in memory (see
[0010](0010-transport-and-protocol.md)). It lets tests and benchmarks repeat exactly, but it does not show what the
server costs when its messages go through real sockets, and it cannot be set against Godot's built-in replication,
which our baseline measured over real sockets in [section 1 of the benchmarks](../benchmarks.md#1-where-godots-built-in-replication-stops-scaling).
That baseline uses `ENetMultiplayerPeer`, so ENet is the transport that makes a comparison fair: both servers send
UDP datagrams through the same library.

The library policy ([0003](0003-library-policy.md)) asks for a permissive license, a build through CMake without manual
installation, few dependencies, an interface of ours in front of it and, on the hot path, a measurement. ENet is MIT
licensed, is one small C library that CMake can fetch, and has no dependencies beyond the system's sockets.

## Decision
- **ENet 1.3.18, behind `ITransport`.** `EnetServerTransport` and `EnetClientTransport` implement the interface in
  [`enet_transport.hpp`](../../core/include/tessera/net/transport/enet_transport.hpp). No other file of ours includes
  an ENet header, and the public header includes none either.
- **Optional.** The code is built only with `-DTESSERA_BUILD_ENET=ON` (off by default). The option downloads ENet with
  `FetchContent` at a fixed tag. With the option off nothing is downloaded, no target is added, and the 495 tests and
  the benchmark lines that do not depend on time are the same as before.
- **Two ENet channels, one per kind of message.** A reliable message is a reliable packet on channel 0. An
  unreliable message is a packet on channel 1 with the `ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT` flag. The flag matters:
  we read `enet_peer_send` in the 1.3.18 source, and without it a packet larger than one datagram is split into
  fragments that ENet sends as reliable ones, so the "unreliable" message would be sent again until it arrived. With the
  flag, losing one fragment loses the whole message, which is what the contract says (and what record 0010 warned
  about for large messages).
- **Connection ids come from a counter of ours**, starting at 1 and never used again, not from ENet's slot number
  (ENet reuses slots). The client sees the server as connection 0.
- **`Disconnect` is `enet_peer_disconnect_later`**, so the messages sent before it still go out first; the side that
  calls it sees nothing more from that connection, as in the simulated network. Only the destructor disconnects at once.
- **`Stats` counts messages; `Wire()` counts what went through the sockets.** `Stats` counts what `Send` accepted and
  what `Poll` handed over. ENet does not say which message a lost packet carried, so `messages_lost` is always 0 and
  the header says so. `Wire()` has ENet's own totals (headers included, UDP and IP not), extended to 64 bits; it is the
  same quantity the Godot baseline reads from its ENet host, and the one the comparison uses.
- **`Flush()`.** The transport hands datagrams to the socket on `Flush()` or on the next `Poll`. The server calls it
  after `Update` and the client after `Poll` (which sends its confirmation).
- **Left out on purpose:** encryption and authentication, reconnecting into an old session, Linux (only Windows has been
  built and measured), and tuning of ENet. ENet's throttle, which can drop unreliable packets when its estimate of the
  link gets worse, stays at its default.

## Consequences
- **The transport is not what limits the server in this scenario, but it is not free.** In the comparison on the
  [benchmarks page](../benchmarks.md#8-tessera-net-and-godots-built-in-replication-same-scenario) (8 clients, every entity
  moving, 20 Hz, localhost, one thread), the part of our tick spent in the transport (reading, sending and flushing) is 0.28
  ms at 500 entities and 1.81 ms at 10,000, which is 45% and 23% of the tick (0.63 and 7.82 ms). The rest of the tick is
  the server's own `Update`. All the sizes keep the 20 Hz tick: at 10,000 entities the server uses about 17% of
  one core.
- **What the comparison shows.** In that scenario, over real sockets, our server used 4.9, 5.9, 7.0 and 5.7 times less
  CPU than the Godot server (median of three alternated rounds at 500, 2,000, 5,000 and 10,000 entities) and sent 4.8, 4.6,
  5.0 and 2.6 times fewer bytes to each client (at 500 entities the operating system's CPU reading of our server is
  unreliable, and the benchmarks page gives a second one). At 10,000 entities the Godot server is at the limit of one core and runs at
  8.6 frames per second instead of 60, so it sends less than the scenario asks for and the ratios there are a floor, not
  a like-for-like figure. Every one of our twelve runs converged: the checksum of each bot's state is the server's, and each
  bot saw every entity.
- **What it does not show.** Part of the byte difference is a design difference, not efficiency alone: Godot sends a
  `Vector3` of floats with its own protocol, while we send positions quantized to 1 cm against the last confirmed state
  ([0009](0009-replicated-state.md)). The two servers also do different work: Godot runs its scene tree and a 30 Hz
  physics step, we run a table and no physics, at a fixed 20 Hz tick. The test is on localhost, with no packet loss and no
  latency, on one machine with background load, on Windows, with only positions replicated. It says that the server and
  the transport work over sockets and what they cost in this scenario, not how a game would behave on the internet.
- **The numbers are on a busy machine.** Programs of everyday work were running and could not be closed; the processor load
  before the runs went from 4.5% to 98.5%, and the short test the benchmark prints (writing one position record) took 52
  to 98 ns against 47 when the machine is quiet. Medians and ranges are on the benchmarks page, so that the spread is
  visible.
- **The default build did not change.** With the option off, the build and the 495 tests are the same; with it on, a
  separate executable, `tessera_net_enet_tests`, holds the 16 tests of the transport (connection and disconnection in
  order, reliable messages whole and in order also with datagrams dropped, a 50,000-byte message on both channels, an
  unreliable message that is not sent again, `Send` to a closed connection, ids never reused, `Stats`, and the
  server converging over ENet). Eight deliberate breakages of the transport were each caught by a test.
- **Left for later:** a transport with encryption and authentication, Linux, a loss and latency test over a real link, and
  the dedicated server that will use this transport. If the ENet library turns out to be a problem on another platform, the
  interface is where it is swapped.
