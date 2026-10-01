# Performance

Numbers come from `bench_book` (`tests/book_bench.cpp`), which compiles `src/book.cpp` at **`-O3 -DNDEBUG`**. The main test binary uses the Debug CMake preset; do not use it to judge throughput.

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

## Reproduce

```bash
cmake --preset ninja
cmake --build --preset ninja
./build/bench_book
```
