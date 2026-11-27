# Performance

Book numbers come from `bench_book` (`tests/book_bench.cpp`), which compiles `src/book.cpp` at **`-O3 -DNDEBUG`**. The main test binary uses the Debug CMake preset; do not use it to judge throughput. End-to-end numbers (exchange and load generator over TCP) are in [End to end](#end-to-end-exchange-and-load-generator) and [many clients](#end-to-end-many-clients-one-symbol-each). How the gateway does client I/O, why it uses io_uring, and a comparison with TigerBeetle are in [Client I/O](#client-io).

A single in-process client flow (`bench_client`), its CPU profile, and the measured effect of the last `Rest` and `SubmitLimitOrder` changes are in [the single-client profile](client-profile.md).

**Design target** ([`prep/requirements.md`](../prep/requirements.md)): **100k–500k+** order operations per second, single thread.

## What is measured

Each workload runs twice, each time on a fresh book:

- **Throughput pass** — the timed loop has no clock calls inside it, so ops/s is not diluted by timer cost.
- **Latency pass** — one `std::chrono::steady_clock` sample per operation (p50, p99, max in ns). Sample storage is sized before the loop. Each sample includes the two clock reads, so p50 reads high for very cheap operations. Max is typically timer or OS noise, not book behaviour.

The workloads are append at one price level (200k rests), rest across 64 price levels (200k rests), cancel random live orders (100k), take one lot from the touch (100k matches), and mixed insert and cancel (300k ops). They stress one path each; a live exchange mix will sit closer to the mixed workload than to the single-level append.

## Sample results

Not recorded yet. The earlier table in this file was measured before fills moved into a fixed buffer and the vectors were replaced by fixed-capacity arrays, so it no longer describes this code. Run `./build/bench_book` on GCC 16+ and paste the output here, with the machine and compiler noted.

| Workload | ops/s | p50 ns | p99 ns | max ns |
|----------|------:|-------:|-------:|-------:|
| Append at one price level | | | | |
| Rest across 64 price levels | | | | |
| Cancel random live orders | | | | |
| Take one lot from the touch | | | | |
| Mixed insert and cancel | | | | |

## End to end: exchange and load generator

`mex_loadgen` (`tools/loadgen.cpp`) drives `mex_exchange` (`src/main.cpp`) over loopback TCP. Both build at **`-O3 -DNDEBUG`** in the `ninja` preset; the exchange keeps its contract checks on. One session sends 1M operations over symbols 1, 2, and 3: about 40% NewOrder (limit, at most 20 ticks from mid, up to 3 ticks through it, so some cross), 40% Cancel, and 20% Replace by `client_order_id`. Each operation gets exactly one final report. Latency is NewOrder -> first report on `steady_clock`, from the time the frame goes into the client's send batch.

Two load modes:

- **Closed loop** (`--window N`): at most N operations without a final report. This measures capacity. The latency is mostly the time spent queued inside the window.
- **Open loop** (`--rate R`): one operation every 1/R seconds, with the default window of 4096.

Measured on 2026-11-07: Intel Core i5-9500 (6 cores, no SMT), Linux 7.2.15, GCC 16.2.1. The exchange and the load generator ran on the same machine. Config: `config/example.cfg` with the port changed (2 engines pinned to cores 2 and 3, the gateway not pinned). Each run used a fresh exchange.

| Load | ops/s | p50 us | p99 us | max us |
|------|------:|-------:|-------:|-------:|
| Closed loop, window 4096 | 6,702,402 | 601.5 | 670.9 | 1375.5 |
| Closed loop, window 64 | 2,392,460 | 22.1 | 47.2 | 317.6 |
| Open loop, 200k ops/s | 199,999 | 27.5 | 81.3 | 564.9 |
| Open loop, 500k ops/s | 499,935 | 2523.0 | 7935.7 | 8259.8 |

The tables in this file are from before 2026-11-10. On that date each order message got a request number and each command an op number (`prep/io-design-spec.md`, "Replication preparation"): `Command` grew from 48 to 64 bytes and the NewOrder, Cancel, and Replace frames by 8 bytes. A comparison on that date, with 5 interleaved runs for each load, showed no end-to-end difference above the run-to-run variation. The machine also ran another exchange, so the tables were not replaced. A single-thread loop of push, `Engine::ProcessOne`, and drain took about 73 ns for each command, against 69 ns before.

Capacity is above the 100k–500k target. The open loop at 500k ops/s holds the rate, but the latency is open: from about 300k ops/s, the reports stop for 2–8 ms at a time, so the window fills and the latency becomes window / rate. During these gaps the gateway and engine threads do not sleep (no voluntary context switches), and the gateway's send buffer does not overflow. The gaps also stay when the gateway, the engines, and the client are on separate cores, with a busy-polling client reader, and with `TCP_QUICKACK`. They occur only when the client sends many small TCP segments. Closed-loop runs send large batches and show no gaps. The cause is not found yet.

Reproduce (change `gateway port` if 9000 is in use):

```bash
cmake --preset ninja && cmake --build --preset ninja
./build/mex_exchange config/example.cfg &
./build/mex_loadgen --port 9000 --symbols 1,2,3 --orders 1000000 --window 64
./build/mex_loadgen --port 9000 --symbols 1,2,3 --orders 1000000 --rate 200000 --session bob --token bob-secret
kill -INT %1
```

## End to end: many clients, one symbol each

Each client is a separate `mex_loadgen` process with its own session and its own symbol. The symbols alternate between 2 engines. `tools/perf_multi_client.sh` runs one case and prints one line.

Setup, measured on 2026-11-08 (same machine and build as above):

- Engines pinned to cores 4 and 5. The exchange process (gateway included) is limited to cores 3–5. The clients are limited to cores 0–2.
- Each value is from 2 runs. Total ops/s is the mean of the 2 runs, with wall time over all clients. p50 is the median client. p99 is the worst client.
- Every run ended with exit code 0 for all processes, with no `SystemBusy` reject, no seq gap, no engine stall, and no dropped report.

Closed loop, window 64 per client, 1M operations per client:

| Clients | Total ops/s | p50 us | p99 us |
|--------:|------------:|-------:|-------:|
| 1 | 1,865,613 | 24 | 46–48 |
| 2 | 2,666,750 | 36 | 62–63 |
| 4 | 2,874,001 | 65–67 | 112–116 |
| 8 | 3,752,018 | 103–111 | 174–204 |

Open loop, one total rate divided equally between the clients:

| Total rate | Clients | p50 us | p99 us |
|-----------:|--------:|-------:|-------:|
| 100k ops/s | 1 | 18–20 | 65–67 |
| 100k ops/s | 4 | 16–17 | 40–344 |
| 100k ops/s | 8 | 19–23 | 54–117 |
| 200k ops/s | 1 | 29–31 | 86–100 |
| 200k ops/s | 4 | 21–22 | 48 |
| 200k ops/s | 8 | 24 | 79–81 |
| 400k ops/s | 1 | 3190–3315 | 9933–9958 |
| 400k ops/s | 4 | 29 | 63–403 |
| 400k ops/s | 8 | 51–54 | 296–610 |

What the numbers show:

- Total throughput increases with the number of clients, but less than linearly. One engine gave the same total as two engines (3.0–3.1M ops/s with 4 clients), so the engines are not the limit. The limit is the one gateway thread, which decodes, validates, routes, and encodes every message, together with 8 client processes on 3 cores.
- The 2–8 ms gaps at 400k ops/s occur with **one** connection. The same total rate on 4 connections has a p99 of 63–403 us. Thus the gaps depend on the message rate on one connection, not on the engines.
- With 8 clients, p99 increases because 8 client processes share 3 cores.

Reproduce:

```bash
cmake --preset ninja && cmake --build --preset ninja
tools/perf_multi_client.sh build 4 1000000 64 0          # closed loop, 4 clients
tools/perf_multi_client.sh build 4 400000 4096 25000     # open loop, 4 x 25k = 100k ops/s
```

## Thread limits and cache-line sharing

Measured on 2026-11-27 with `perf` (same machine, Linux 7.2.15, perf 7.2.11,
`perf_event_paranoid` 2). The load was `tools/perf_multi_client.sh` with 4
clients, closed loop. `perf` attached to the exchange for 3–5 s of steady load.

| Window | Total ops/s | Engine CPU | Gateway user / kernel | 4 clients on 3 cores |
|-------:|------------:|-----------:|----------------------:|---------------------:|
| 64 | 3.3M | 100% each (spin) | 19% / 43% | saturated |
| 4096 | 6.0M | 100% each (spin) | 36% / 36% | 286% of 300% |

- The clients are the limit on this 6-core host. The gateway has 28–38% idle
  time, and each engine is busy only about 12% of its time. So a faster
  exchange does not show as more throughput in these tests. Measure engine work
  with `bench_engine` (below).
- `perf c2c` found no false sharing. All hot lines have HITM loads at offset 0
  only: the engines read the inbound `SpscQueue::tail_`, and the gateway reads
  the outbound `tail_`. That is the true sharing that the queue design expects.
  Command and report slots share lines one at a time, also as designed.
- In the engine, `ClientOrderIndex::Find` has most DRAM loads. Each new
  client order ID hashes to a random line of the index. With `max_orders`
  1,000,000 instead of 4,096, this adds about 13 ns to each command.

How to repeat: the `perf-profiling` and `perf-false-sharing` skills in
[ai-assets](https://github.com/breakbadsp/ai-assets) give the commands.

## Engine thread

`bench_engine` (`tests/engine_bench.cpp`) feeds one engine from its own thread
with the `mex_loadgen` order mix, and reports the mean time for each command,
from the inbound queue to its reports. It builds like `mex_exchange` (-O3,
contracts on). Each value is the median of 5 runs of 4M commands on core 4.

| `max_orders` | Market data | Before | After |
|-------------:|------------:|-------:|------:|
| 4,096 | off | 102 | 106 |
| 4,096 | on | 206 | 107 |
| 1,000,000 | off | 115 | 118 |
| 1,000,000 | on | 220 | 119 |

Values are ns per command. "Before" published the depth after every command.
`MarketBook::Publish` then used about a quarter of all engine cycles, so the
feed halved the capacity of each engine. "After" publishes each changed book
once per batch (`kMarketDataBatch`). The "off" rows did not change; their
difference is run-to-run variation.

```bash
taskset -c 4 ./build/bench_engine
```

### Market depth capture

`bench_engine --depth-capture` times the capture and handoff of a book with
20 visible levels and one hidden level per side. On the i5-9500 with GCC
16.2.1 and contracts enabled, the median fell from 95.6 ns to 28.2 ns per
capture. The benchmark used five interleaved runs for each build on core 4,
with each run reporting its median over five sets of 4M captures. Three
interleaved runs of the regular engine benchmark showed no clear change in
time per command. [Lean verification](../verification/Lean/README.md)
checks the traversal and gives the benchmark commands.

## Client I/O

### How the gateway does client I/O

One gateway thread does all socket I/O with io_uring. The engines make no system calls; they only use their SPSC queues. The ring has one multishot accept, one single-shot `recv` and at most one `send` in flight for each connection. It does not use SQPOLL, multishot receive, or registered buffers. Each loop (`Gateway::PollOnce` in `src/gateway.cpp`) does these steps:

1. Submit the queued SQEs, if any, then busy-poll the completion queue. The thread never sleeps. It uses one full core, and it does not pay a wake-up cost.
2. For each accepted connection: `TCP_NODELAY`, a slot in the fixed connection table, and a `recv` into its read buffer (64 frames).
3. For each completed `recv`: decode every complete frame, validate it, and push it to the engine's inbound queue. Move the partial frame to the start of the buffer, then arm the next `recv`.
4. Pop the reports from each engine's outbound queue. Encode each report with the session's next seq into the connection's send buffer (1024 frames).
5. For each connection with data and no send in flight: queue one `send` for all the reports of this loop. If a report does not fit, the gateway writes what the kernel takes now with a direct non-blocking `send`, unless a send is in flight. If it still does not fit, the gateway disconnects the client (slow consumer).

Buffer ownership:

- While a send is in flight, the kernel reads those bytes, so the gateway does not move them. A send is still in flight during the outbound drain only if the socket buffer was full when the kernel tried it. Reports appended meanwhile must fit behind it in the 1024-frame send buffer, or the client is disconnected as a slow consumer.
- A closed connection's slot is reused only after its recv and send have completed: Close cancels both (by user data) and closes the fd, and the slot returns to the free list on the last completion. Until then, a new connection beyond `max_connections` is closed.
- `Run()` cancels all operations after `Stop()` and waits for them, so the buffers outlive every kernel access.

The kernel must permit io_uring. The exchange stops at startup with an `io_uring_queue_init` error if `kernel.io_uring_disabled` or a seccomp profile refuses it. The default Docker seccomp profile refuses it; CI runs its container without that profile.

### Why io_uring only

The gateway had an epoll backend too, and it was the default. These numbers come from `tools/perf_multi_client.sh` on one machine, 2 runs for each case, with both backends:

| Case | epoll | io_uring |
|------|------:|---------:|
| Closed loop, 1 client, total ops/s | 1,865,613 | 1,807,021 |
| Closed loop, 2 clients, total ops/s | 2,666,750 | 2,581,728 |
| Closed loop, 4 clients, total ops/s | 2,874,001 | 3,205,450 |
| Closed loop, 8 clients, total ops/s | 3,752,018 | 3,696,557 |
| Open loop 100k ops/s, 4 clients, p99 us | 40–344 | 32–34 |
| Open loop 200k ops/s, 8 clients, p99 us | 79–81 | 97–142 |
| Open loop 400k ops/s, 1 client, p99 us | 9933–9958 | 9969–10012 |

Throughput is about equal: epoll is 2–3% faster with 1 or 2 clients, and with more clients the run-to-run variation is larger than the difference. The system calls are not the main cost. Each small frame needs the same decode, validation, routing, and encode work on the gateway thread.

io_uring is kept, and epoll is removed, for these reasons:

- Two backends doubled the I/O paths to test and reason about, for no measured gain.
- The io_uring features that can reduce the cost for each message (SQPOLL, multishot receive with provided buffers, registered files and buffers) need the completion model. epoll has no path to them.

Measure again before you use a result from this table: it predates later gateway changes.

### Comparison with TigerBeetle

TigerBeetle (`src/message_bus.zig`, `src/stdx/io/linux.zig`, `src/message_buffer.zig` on its `main` branch) also does client I/O on one thread, but its design is different:

| | This exchange | TigerBeetle |
|---|---|---|
| I/O model | completion (io_uring), one `recv` and at most one `send` in flight for each connection | completion (io_uring): the kernel copies into buffers given in advance |
| Wait | busy-polls the completion queue | blocks in `io_uring_enter` until a completion or a deadline |
| Message | 23–48 bytes, one order or report | up to `message_size_max` (MiB scale), up to 8,190 events |
| Requests in flight for each client | many | one |
| Receive buffer for each connection | 64 frames | one full-size message |
| Send | one byte buffer for each connection, one `send` for all reports of a loop | each message sent from its pool buffer; tries a direct non-blocking send first, then io_uring |
| Send queue full | disconnect the client | drop the message; the client retries its request |
| Input check | type, exact length, enum values | header and body checksums, size, command |
| Sessions | fixed list from config, logon with a token | registered at run time, limit `clients_max`, oldest session evicted |

Why the designs differ:

- **Messages per system call.** TigerBeetle puts thousands of events in one message, so the cost of a system call is shared by many events. This exchange receives one order in each frame, so the gateway's work for each message is the main cost. This is why changing epoll to io_uring did not change our throughput.
- **Latency target.** Our target is microseconds, so the gateway busy-polls and uses one core all the time. TigerBeetle waits for disk writes and replica agreement, so a wake-up of some microseconds is not important, and it does not use a full core.
- **Full send queue.** Our reports carry a seq number and v1 has no resend, so the gateway must not drop a report; it disconnects the client. TigerBeetle can drop a reply, because each client has one request in flight and retries it until it gets a reply.
- **Buffer lifetime.** With a completion model, the kernel owns a buffer until the operation completes. TigerBeetle and our gateway both wait for all I/O in flight on a connection before they use its slot again.

Possible next steps, in order of expected effect:

1. Let one frame carry many orders (a protocol change), so that one system call and one validation pass cover more orders.
2. More than one gateway thread (already planned in `prep/io-design-spec.md`).
3. io_uring features that neither system uses now: SQPOLL, multishot receive with provided buffers, and registered files and buffers. SQPOLL needs a spare core.

## Reproduce (book benchmark)

```bash
cmake --preset ninja
cmake --build --preset ninja
./build/bench_book
```
