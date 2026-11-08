# modern_exchng_cpp

Single-symbol limit order book with price-time (FIFO) matching. One `mex::Book` instance holds resting limits in a pre-sized pool and sorted price levels; market orders match immediately and never rest.

## Features

- **Limit and market** orders, partial fills, cancel by internal order id (pool index)
- **Fixed capacity** at construction (`max_orders`, `max_price_levels`); full pool or ladder rejects instead of growing
- **Single-threaded**, no heap allocation on the hot path after setup
- Prices are **integer ticks** (`mex::Price`); order sizes are `mex::Quantity` (`uint32_t` units). Level totals stay `uint64_t`.

Design background and locked decisions: [`prep/design-spec.md`](prep/design-spec.md). Gateway, queues, and engine threads: [`prep/io-design-spec.md`](prep/io-design-spec.md). C++ style: [`docs/naming-conventions.md`](docs/naming-conventions.md) (2-space indent, Allman braces; enforced by [`.clang-format`](.clang-format)).

## Requirements

- C++26 compiler: **GCC 16+** with working `-fcontracts` (configure fails otherwise). Clang is rejected.
- [CMake](https://cmake.org/) 3.20+
- [Ninja](https://ninja-build.org/) (required generator)
- [Google Test](https://github.com/google/googletest) (system package, e.g. `gtest` on Arch)

## Build and test

From the repo root (point CMake at GCC 16+ if that is not the default `c++`):

```bash
cmake --preset ninja
cmake --build --preset ninja
ctest --preset ninja --output-on-failure
```

AddressSanitizer and UndefinedBehaviorSanitizer (together):

```bash
cmake --preset ninja-asan-ubsan
cmake --build --preset ninja-asan-ubsan
ctest --preset ninja-asan-ubsan --output-on-failure
```

Or pass `-DMEX_ENABLE_ASAN=ON` and/or `-DMEX_ENABLE_UBSAN=ON` to a Ninja configure.

Binaries land in `build/` (or `build-asan-ubsan/`): static library `libmex.a`, the tests, benchmarks `bench_book` and `bench_client`, the exchange `mex_exchange`, and the load generator `mex_loadgen`.

Configure fails if CMake is not using Ninja (`-G Ninja` or the preset above), if the compiler is not GCC 16+, or if `-fcontracts` does not compile.

## Run the exchange

`mex_exchange` reads one config file ([`config/example.cfg`](config/example.cfg) documents the syntax), starts one pinned thread per engine and a gateway thread, and stops on SIGINT or SIGTERM. A core that cannot be pinned is a warning, not an error.

```bash
./build/mex_exchange config/example.cfg
```

`mex_loadgen` logs on as one session, sends a mix of NewOrder, Cancel, and Replace, and prints throughput and NewOrder -> first report latency. It exits non-zero if logon fails, a seq gap is seen, or the connection fails. `--help` lists the options.

```bash
./build/mex_loadgen --port 9000 --session alice --token alice-secret --symbols 1,2,3 --orders 1000000
```

The session owns its resting orders, so a second run on the same session and exchange can get `DuplicateClientOrderId` rejects. Restart the exchange or use another session for clean numbers.

## Formatting

Requires `clang-format` on `PATH`. After configure:

```bash
cmake --build build --target format              # rewrite sources to match .clang-format
cmake --build build --target clang-format-check  # verify (non-zero if drift)
```

## Performance

Design target: **100k–500k+** order ops/s on one thread ([`prep/requirements.md`](prep/requirements.md)). `bench_book`, `mex_exchange`, and `mex_loadgen` build at **`-O3`** (tests use Debug).

```bash
./build/bench_book
```

`bench_book` prints throughput (ops/s) from a loop with no per-op clock calls, plus
p50 / p99 / max latency from a separate per-op-timed pass. Rerun locally after changes.

Methodology, the end-to-end result, and full table: [`docs/performance.md`](docs/performance.md).

`bench_client` runs one in-process client flow through the book API. Its results, CPU
profiles, and the profiling scripts: [`docs/client-profile.md`](docs/client-profile.md).

## Layout

```
src/          Book, protocol, queue, engine, gateway, config, and the exchange main
tools/        load generator client
scripts/      benchmark comparison and CPU profiling of the book
tests/        Google Test suites, the end-to-end smoke test, and book_checks helpers
prep/         design spec and requirements
docs/         naming and C++ standards reference
```

## Library usage (sketch)

```cpp
#include "book.hpp"

mex::Book book(max_orders, max_price_levels);

mex::SubmitResult r =
    book.SubmitLimitOrder(mex::Side::Buy, mex::Price{100}, mex::Quantity{10});
// r.status_, r.filled_qty_, r.remaining_, r.order_id_
// book.Fills() is the maker fills from this submit only.
// If fill.maker_removed_ is true, do not call Order(fill.maker_id_):
// matching freed that slot and Rest may reuse it in the same submit.
// Use fill.side_, fill.price_, and fill.quantity_ instead.

book.CancelOrder(r.order_id_);
```

Public API lives in `src/book.hpp`; include directory is `src/` when linking against `mex`.

## TODO

The work above `Book`: gateway, queues, and engine threads. Design and open
decisions: [`prep/io-design-spec.md`](prep/io-design-spec.md).

- [x] `Book`: matching core (limit, market, cancel, partial fills)
- [x] `spsc_queue.hpp`: ring buffer plus a two-thread stress test
- [x] `protocol.hpp`: wire structs, encode/decode, partial-frame handling; unit tests without sockets
- [x] `engine.hpp/.cpp`: driven through its queues in tests, no network; covers id generation, ownership checks, and maker-before-claim
- [x] `gateway.hpp/.cpp`: epoll, connection table, routing, writes
- [x] `main.cpp` + a load-generator client for end-to-end throughput against the 100k–500k ops/s target

## License

Not specified yet.
