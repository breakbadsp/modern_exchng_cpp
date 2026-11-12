# Single-client CPU profile

`bench_client` (`tests/client_bench.cpp`) runs one synchronous client through
the public `Book` API. Each cycle sends eight requests:

1. Rest a two-unit bid at an existing price. Cycle through all bid prices.
2. Cancel that bid.
3. Insert a new best ask for three units.
4. Buy one unit with a crossing limit order. Leave a partial maker.
5. Buy the remaining two units with a market order.
6. Sell one unit at the best bid with a market order.
7. Refill that bid.
8. Reject an invalid cancel.

After each submit the client reads `Fills()`. Each run checks the fill count,
quantity, and notional, the cancel counts, the free slots, the ladder depth,
and the quantities at the best prices, and fails if one is wrong. Each cycle
restores the seeded book. Construction and 20,000 warm-up cycles are not
timed. The build is `-O3 -DNDEBUG`, contracts `ignore`, frame pointers.

This is the in-process book only: no network, parser, or queue. Batch ns per
request is an average over the loop, not a latency percentile.

## Tools

CPU counters and call-graph samples now start after construction and warm-up,
and stop before validation and output. The scripts start perf disabled and use
an acknowledged control channel with `bench_client perf`. The window includes
fixed control and timer overhead; use long runs to amortize it. The comparison
tool requires at least 10,000 counter cycles. Old recordings include startup
and warm-up; their batch timings remain valid, but their counter normalization
is approximate.

- `scripts/compare_client.py` builds each variant with the same CMake rules
  and the same `tests/`; only `src/` and LTO differ. A variant source is
  `work`, a git commit, or a directory of replacement `src/` files. Each
  round runs every variant at 1, 64, and 1,024 levels, then `bench_book`,
  and rotates the variant order. It also records latency runs and
  `perf stat` counters.
- `scripts/profile_client.py` builds the working tree, then records
  latency runs, `perf stat`, and `perf record` call graphs.
- `scripts/profile_graph.py DIRECTORY` draws a call graph from one
  recording. It charges inlined code to the function that contains it. With
  LTO, most book code is inlined into the client loop, so use a recording
  made with `--lto off` for per-function cost.

The graph tool writes DOT, SVG, JSON, and a standalone HTML viewer. Paths are
escaped before use in DOT and HTML. The short graph caption keeps node labels
readable. For the current run:

```sh
python3 scripts/profile_client.py --cpu 2 --lto off --output build/client_profile_fresh
python3 scripts/profile_graph.py build/client_profile_fresh --levels 64
```

The fresh run on the machine below measured 15.16 ns/request as a median batch
average at 64 levels, and 14.99 CPU ns/request in the counter run. In 2,402
cycle-weighted samples, `Rest` used 45.1% self / 48.4% total, `Match` 13.1%,
and cancel plus its level lookup 9.4%. Contracts and LTO were disabled. These
are in-process measurements and sample estimates, not network latency or exact
function timers. The output folder contains the raw data and build metadata.

Each tool writes a new directory under `build/` and refuses an existing
`--output`, so earlier results stay. LTO builds pass
`-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` in their own build directory; the
preset build does not change.

## Results

### User and system CPU time

Run `cmake --build build --target bench_client`, then
`python3 scripts/profile_syscalls.py --cpu 2` to repeat the check. The benchmark
reads thread user/system CPU counters after warm-up and after the measured
window. Five untraced runs of 160 million requests each used 99.9798% user CPU
and 0.0202% system CPU in aggregate. Thread accounting has microsecond resolution.

A separate `strace` run covered eight million requests. Between the two usage
reads it found only two `clock_gettime(CLOCK_PROCESS_CPUTIME_ID)` calls from
the timing harness. No per-order syscalls were observed. File access, memory
mapping, and affinity calls occurred outside that window. Normal steady-clock
reads used the vDSO and did not enter the kernel. These results cover the book,
not the network gateway. Raw output is in `build/client_syscalls_fresh/`.

Measured on 2026-11-08: Intel Core i5-9500, Linux 7.2.15, GCC 16.2.1,
pinned to CPU 2. The `powersave` governor and turbo were not controlled.
The base is `origin/master` at `b2965a0`. 15 interleaved rounds of
8 million requests per run.

The changes:

1. `Rest` searches the ladder once and checks for room before it changes
   the book. It returns `kInvalidOrderId` when the remainder cannot rest.
   `CanRest` is removed. Matching still runs first, because a fill can free
   the slot or the level that the remainder needs.
2. `SubmitLimitOrder` calls `Match` only when the best opposite price
   crosses. `Match` stopped at that price in the other case, so results do
   not change.

Client flow, median batch ns per request. Ratio is the median per-round
ratio of changes 1-2 to the base, both without LTO.

| Levels | Base | Change 1 | Changes 1-2 | Ratio | Changes 1-2, LTO |
|---|---:|---:|---:|---:|---:|
| 1 | 13.49 | 12.85 | 12.11 | 0.902 | 10.85 |
| 64 | 19.05 | 16.97 | 16.08 | 0.844 | 14.54 |
| 1,024 | 26.69 | 20.90 | 19.75 | 0.740 | 18.24 |

Median per-round ratio for each step (lower is faster), and the number of
the 15 rounds in which the first variant was faster:

| Step | 1 level | 64 levels | 1,024 levels |
|---|---:|---:|---:|
| Change 1 / base, no LTO | 0.962 (14) | 0.900 (15) | 0.783 (15) |
| Change 2 / change 1, no LTO | 0.947 (15) | 0.947 (15) | 0.944 (15) |
| Change 2 / change 1, LTO | 1.000 (8) | 1.002 (6) | 0.984 (13) |
| `std::optional` return / change 1, no LTO | 1.106 (1) | 1.024 (2) | 1.021 (3) |
| LTO / no LTO, base | 0.915 (15) | 0.969 (14) | 1.006 (3) |
| LTO / no LTO, changes 1-2 | 0.892 (15) | 0.904 (15) | 0.925 (15) |

Cycles and instructions per request, base → changes 1-2 without LTO:
57.8 / 186 → 52.4 / 159 at 1 level, 81.7 / 243 → 69.1 / 192 at 64, and
116.0 / 285 → 85.4 / 215 at 1,024. Branch misses at 1,024 levels fell from
1.26 to 0.75 per request.

`bench_book` throughput pass, median ns/op, base → changes 1-2 without LTO
(median per-round ratio): append 11.71 → 7.60 (0.648), rest across 64
levels 20.00 → 11.66 (0.582), random cancel 30.95 → 30.98 (1.003), take one
lot 12.83 → 13.04 (1.036), mixed 50.72 → 43.38 (0.848).

Self share of user cycles after changes 1-2, without LTO (`perf record`,
1,000-3,000 samples per depth, so each share is an estimate):

| Function | 1 level | 64 levels | 1,024 levels |
|---|---:|---:|---:|
| `Book::Rest` (with its inlined ladder search) | 28% | 42% | 39% |
| Client loop (`ClientFlow::Step`) | 23% | 18% | 14% |
| `Book::Match` | 19% | 14% | 11% |
| `Book::SubmitLimitOrder` | 11% | 8% | 9% |
| `Book::LevelIndex` + `Book::CancelOrder` | 7% | 9% | 21% |

`LevelIndex` is the ladder search in `CancelOrder`: cancel finds the level
by price, although it has the order id. A stable level handle per order
could remove that search. Level insert and erase move the levels, so an
index alone is not stable; measure the cost to keep a handle correct before
you adopt one.

Costs and limits:

- For a limit order that crosses, change 2 adds a price check that
  `Match` then repeats. Take one lot (every order crosses) was 4% slower
  than change 1 alone without LTO (slower in all 15 rounds), and 10%
  slower with LTO (slower in 13 of 15). With LTO, change 2 gave no gain in
  the client flow. It is kept because the shipped build (`mex_exchange`) does
  not use LTO, and the client flow has three resting limit orders to one
  crossing order. A flow of mostly crossing limit orders will be slower.
- `Rest` returns the `kInvalidOrderId` sentinel, which is also the
  `SubmitResult` value for "nothing rests". A `std::optional<OrderId>`
  return was slower in the client flow (table above) and in `bench_book`
  append (1.5×). The cause was not analyzed.
- LTO is a measurement option, not a default: the shipped build does not
  use it. With LTO, `bench_book` on the base was slower in each workload
  (median ratio 1.03-1.12). This was not analyzed; `bench_book` runs each
  workload once, for a few milliseconds.
- One machine. Interleaved rounds and per-round ratios reduce drift but do
  not remove it.
- The flow is warm, with short queues. It does not cover multi-maker sweeps,
  cold caches, full-capacity rejects, or contract checks on.

Reproduce (each run builds its variants in the new output directory):

```sh
python3 scripts/compare_client.py --cpu 2 \
  --variant base b2965a0 off --variant base_lto b2965a0 on \
  --variant work work off --variant work_lto work on
python3 scripts/profile_client.py --cpu 2 --lto off
```

The step variants were directories with `src/book.cpp` and `src/book.hpp`
that had only change 1, or change 1 with a `std::optional` return, passed
as `--variant NAME DIRECTORY on|off`.

## History: commit 5c5f99a

The same two changes were first measured on `5c5f99a`. That code had
`std::vector` ladders and a function-pointer fill callback in place of
`Fills()`. There, changes 1-2 with LTO measured 23%, 25%, and 32% faster at
1, 64, and 1,024 levels. That API no longer exists, so these numbers do not
describe the current code. The table above replaces them.
