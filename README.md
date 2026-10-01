# modern_exchng_cpp

Single-symbol limit order book with price-time (FIFO) matching. One `mex::Book` instance holds resting limits in a pre-sized pool and sorted price levels; market orders match immediately and never rest.

## Features

- **Limit and market** orders, partial fills, cancel by internal order id (pool index)
- **Fixed capacity** at construction (`max_orders`, `max_price_levels`); full pool or ladder rejects instead of growing
- **Single-threaded**, no heap allocation on the hot path after setup
- Prices are **integer ticks** (`mex::Price`); order sizes are `mex::Quantity` (`uint32_t` units). Level totals stay `uint64_t`.

Design background and locked decisions: [`prep/design-spec.md`](prep/design-spec.md). C++ style: [`docs/naming-conventions.md`](docs/naming-conventions.md) (2-space indent, Allman braces; enforced by [`.clang-format`](.clang-format)).

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

Binaries land in `build/` (or `build-asan-ubsan/`): static library `libmex.a`, tests `test_book`, benchmark `bench_book`.

Configure fails if CMake is not using Ninja (`-G Ninja` or the preset above), if the compiler is not GCC 16+, or if `-fcontracts` does not compile.

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

`bench_book` prints throughput (ops/s) from a loop with no per-op clock calls, plus
p50 / p99 / max latency from a separate per-op-timed pass. Rerun locally after changes.

Methodology and full table: [`docs/performance.md`](docs/performance.md).

## Layout

```
src/          book types (`types.hpp`) and Book (`book.hpp`, `book.cpp`)
tests/        Google Test suites and book_checks helpers
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

## License

Not specified yet.
