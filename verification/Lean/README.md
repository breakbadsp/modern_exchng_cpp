# Verify matching and market depth with Lean

Install the Lean release in [lean-toolchain](lean-toolchain). With Elan, the
test selects the toolchain from this directory. A build can also use a
standalone release through `-DMEX_LEAN_EXECUTABLE=/path/to/bin/lean`. Use this
build for KaizenFlow checks that cover matching or market data.

```sh
cmake --preset ninja -DMEX_VERIFY_LEAN=ON
cmake --build --preset ninja --target lean_matching_replay lean_depth_replay
ctest --preset ninja -R '^lean_verification$' --output-on-failure --no-tests=error
```

The Lean model uses a stable price sort over orders in arrival order. The
replay compares acceptance, fills in execution order, remaining quantity,
and resting orders in FIFO order after each command. Named traces cover FIFO,
price sweeps, and capacity. Seeded traces vary commands, prices, quantities,
and limits. A failure identifies the trace and step so you can repeat it.

The depth oracle groups resting orders by price and sums their quantities.
The C++ replay submits and cancels orders, then compares the view from the
real market data handoff. It covers empty and full sides, hidden levels,
promotion after a cancel, and quantities that exceed 32 bits in aggregate.

Lean proves the model's fill quantity bound and the equivalence between
walking backward through the captured suffix and taking the best-first
prefix. The replay tests connect these models to the C++ behavior over the
generated inputs. Use [TLA+](../TLA/README.md) for bounded concurrency and
protocol checks.

Run the focused benchmark on a quiet CPU before and after a capture change.
Compare the median of several interleaved runs. The second command checks
the order mix with market data on and off.

```sh
cmake --build --preset ninja --target bench_engine
taskset -c 4 build/bench_engine --depth-capture 4000000
taskset -c 4 build/bench_engine 4000000
```
