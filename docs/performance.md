# Performance

Numbers come from `bench_book` (`tests/book_bench.cpp`), which compiles `src/book.cpp` at **`-O3 -DNDEBUG`**. The main test binary uses the Debug CMake preset; do not use it to judge throughput.

**Design target** ([`prep/requirements.md`](../prep/requirements.md)): **100k–500k+** order operations per second, single thread.

## Sample results

Measured on one Linux/x86_64 run (GCC, `-O3`). Your machine will differ; always rerun `./build/bench_book` after changes.

| Workload | ops/s | ns/op |
|----------|------:|------:|
| Append at one price level (200k rests) | 62.5M | 16.0 |
| Rest across 64 price levels (200k rests) | 44.8M | 22.3 |
| Cancel random live orders (100k) | 30.0M | 33.4 |
| Take one lot from the touch (100k matches) | 63.9M | 15.6 |
| Mixed insert and cancel (300k ops) | 18.8M | 53.1 |

Synthetic micro-benchmarks stress one path each. A live exchange mix (spread updates, depth, rejects) will sit closer to the mixed workload than the single-level append case.

## Reproduce

```bash
cmake --preset ninja
cmake --build --preset ninja
./build/bench_book
```
