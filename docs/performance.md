# Performance

Book numbers come from `bench_book` (`tests/book_bench.cpp`), which compiles `src/book.cpp` at **`-O3 -DNDEBUG`**. The main test binary uses the Debug CMake preset; do not use it to judge throughput. End-to-end numbers (exchange and load generator over TCP) are in [End to end](#end-to-end-exchange-and-load-generator).

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

Capacity is above the 100k–500k target. The open loop at 500k ops/s holds the rate, but the latency is open: from about 300k ops/s, the reports stop for 2–8 ms at a time, so the window fills and the latency becomes window / rate. During these gaps the gateway and engine threads do not sleep (no voluntary context switches), and the gateway's send buffer does not overflow. The gaps also stay when the gateway, the engines, and the client are on separate cores, with a busy-polling client reader, and with `TCP_QUICKACK`. They occur only when the client sends many small TCP segments. Closed-loop runs send large batches and show no gaps. The cause is not found yet.

Reproduce (change `gateway port` if 9000 is in use):

```bash
cmake --preset ninja && cmake --build --preset ninja
./build/mex_exchange config/example.cfg &
./build/mex_loadgen --port 9000 --symbols 1,2,3 --orders 1000000 --window 64
./build/mex_loadgen --port 9000 --symbols 1,2,3 --orders 1000000 --rate 200000 --session bob --token bob-secret
kill -INT %1
```

## Reproduce (book benchmark)

```bash
cmake --preset ninja
cmake --build --preset ninja
./build/bench_book
```
