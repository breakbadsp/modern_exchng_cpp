# modern_exchng_cpp

Single-symbol limit order book with price-time (FIFO) matching. One `mex::Book` instance holds resting limits in a pre-sized pool and sorted price levels; market orders match immediately and never rest.

## Features

- **Limit and market** orders, partial fills, cancel by internal order id (pool index)
- **Fixed capacity** at construction (`max_orders`, `max_price_levels`); full pool or ladder rejects instead of growing
- **Single-threaded**, no heap allocation on the hot path after setup
- Prices are **integer ticks** (`int64_t`); quantities are `uint32_t`

Design background and locked decisions: [`prep/design-spec.md`](prep/design-spec.md). C++ style: [`docs/naming-conventions.md`](docs/naming-conventions.md) (2-space indent, Allman braces; enforced by [`.clang-format`](.clang-format)).

## Requirements

- C++23 compiler (GCC or Clang)
- [CMake](https://cmake.org/) 3.20+
- [Ninja](https://ninja-build.org/) (required generator)
- [Google Test](https://github.com/google/googletest) (system package, e.g. `gtest` on Arch)

## Build and test

From the repo root:

```bash
cmake --preset ninja
cmake --build --preset ninja
ctest --preset ninja --output-on-failure
```

Binaries land in `build/`: static library `libmex.a`, tests `test_book`, benchmark `bench_book`.

Configure fails if CMake is not using Ninja (`-G Ninja` or the preset above).

## Formatting

Requires `clang-format` on `PATH`. After configure:

```bash
cmake --build build --target format              # rewrite sources to match .clang-format
cmake --build build --target clang-format-check  # verify (non-zero if drift)
```

## Performance

Design target: **100k–500k+** order ops/s on one thread ([`prep/requirements.md`](prep/requirements.md)). `bench_book` builds the book at **`-O3`** (tests use Debug).

```bash
./build/bench_book
```

Example on Linux/x86_64 (GCC, `-O3`; rerun locally after changes):

| Workload | Throughput |
|----------|-------------|
| Append at one price | ~62M ops/s |
| Rest across 64 levels | ~45M ops/s |
| Cancel (random order) | ~30M ops/s |
| Take one lot at touch | ~64M ops/s |
| Mixed insert + cancel | ~19M ops/s |

Methodology and full table: [`docs/performance.md`](docs/performance.md).

## Layout

```
src/          types, OrderNode, PriceLevel, Book (headers + book.cpp)
tests/        Google Test suites and book_checks helpers
prep/         design spec and requirements
docs/         naming and C++ standards reference
```

## Library usage (sketch)

```cpp
#include "book.hpp"

mex::Book book(max_orders, max_price_levels);

auto on_fill = [](mex::OrderId p_maker_id, std::int64_t p_price, std::uint32_t p_qty) {
    // record trade
};

mex::SubmitResult r = book.SubmitLimitOrder(mex::Side::Buy, 100, 10, on_fill);
// r.status_, r.filled_qty_, r.remaining_, r.order_id_

book.CancelOrder(r.order_id);
```

Public API lives in `src/book.hpp`; include directory is `src/` when linking against `mex`.

## License

Not specified yet.
