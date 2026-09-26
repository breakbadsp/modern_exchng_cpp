# HFT Matching Engine — Requirements

## Environment / Toolchain

- C++, compiled with both latest GCC and latest Clang
- Arch Linux (Omarchy), all latest versions
- Latest available `std::c++` standard supported by both compilers
(currently C++23 in practice — C++26 features like contracts/reflection
are GCC-only right now, so held back)

## Scope

- Equity asset class only
- Order types: Market and Limit only — no multi-leg/spread orders
- Single symbol per order book instance
- Operations: New order, Cancel, Replace (price and/or quantity
amendment), partial fills
- FIFO (price-time) priority for orders at the same price



## Functional / non-negotiable behavior

- Correctness of FIFO ordering must hold under cancel and replace, not
just plain inserts
- No hidden state corruption from optimizations (e.g. no silent
bookkeeping errors like the lazy-cancel issue flagged earlier)



## Performance targets

- 100k–500k+ orders/sec sustained, on a single thread
- Prioritize cache locality and predictable (branch-friendly) code paths
over asymptotic complexity where the two trade off



## Hard constraints

- Single-threaded matching core — no concurrency in v1, kept simple
deliberately
- No heap allocation once the system is running — everything
sized/allocated up front
- Minimize allocation-like data movement even within pre-allocated
storage, where practical (accepted as a conscious trade-off in a few
places rather than an absolute rule)
- Order pool capacity: ~1,000,000 orders, statically/globally allocated
(not stack-resident)



## Guiding principle

- Simplicity is a first-order goal, not an afterthought — repeatedly
traded against raw performance, with the trade made explicit each time
rather than assumed

