# MEX TLA+ verification

This project checks bounded subsystem specifications. A wrong event order can
change a trade, assign a report to the wrong order, or prevent shutdown.
TLC checks the models. Replay and CTest check the production implementation.

## Run

Build MEX normally when you need implementation checks that use the exchange:

    cmake --preset ninja -DMEX_LIBVSR_SOURCE_DIR=/path/to/libvsr
    cmake --build --preset ninja

Run one check, replay a scenario, or run the full suite:

    verification/TLA/run list
    verification/TLA/run check matching
    verification/TLA/run check bridge --build build
    verification/TLA/run replay bridge verification/TLA/bridge/retry.json --build build
    MEX_LIBVSR_SOURCE_DIR=/path/to/libvsr verification/TLA/run check --all --build build

Python, Java, and curl are required for TLC. The runner downloads and checks
the SHA-256 of TLA+ 1.8.0. Matching replay also requires GCC 16 or newer.
Set JAVA, TLC_WORKERS, or CXX to change the executable or worker count.
The libvsr path must be a Git checkout with the commit pinned in
CMakeLists.txt. The runner reads the model at that commit.

The runner prints a separate evidence directory for each invocation.
Logs, model-generated scenarios, and scratch executables stay under .tools/runs/.
An implementation check fails if the build lacks any mapped CTest entry.
Witness checks intentionally reach a named invariant violation. A mutation
passes only if TLC reports the named violation. Compilation, parsing, and
tool failures fail the check.

## Scope and proof

| Model | State boundary | Implementation evidence |
| --- | --- | --- |
| Matching | One symbol. Logical order identities and FIFO position. Separate general and full-ladder configurations. | Direct Book replay of hand-written scenarios and TLC witnesses for reduce, a multi-price sweep, and full-ladder replace. A scratch C++ mutation changes reduction into replacement. |
| Engine | Live quantity, cumulative fills, abstract per-order generation, and request high-water marks. Applies one-symbol order lifecycle rules with an abstract fill. | Existing engine tests cover reports, cross-session ownership, physical slot reuse, deduplication, and snapshot restore. |
| Bridge | One intake FIFO, one pending batch, timeout, reply, and eviction. | Direct BasicReplicationBridge::Step() replay of a hand-written case and a TLC eviction witness. Existing replication tests cover multiple intake queues, batch overflow, and real replicas. |
| Recovery | Committed prefix, snapshot, one restart, an abstract state value, and the report cutoff. | Existing replication and engine tests check real snapshots, replay, and clock skew. |
| Gateway | Logon attaches a session, queues the logon acknowledgement, and starts report sequence numbering. Slots track generation and kernel buffer ownership. | Existing socket tests exercise both backends. The model explores explicit completion orders. |
| Frames | Abstract header and body tokens. A valid frame is consumed once. Malformed input closes the connection when processing runs. | Existing codec and socket tests exercise real byte buffers. |
| Flow | One command producer, one report reader, two bounded queues, and stop and join. | Existing SPSC stress tests, engine stall tests, and standalone and replicated smoke tests. TLC assumes enabled publish, drain, join, and stop actions eventually run. |
| SpscMemory | Separate write, release, acquire, read, and slot reuse steps. An acquire makes writes before its observed release visible. | Existing two-thread SPSC tests. The model also catches a tail refresh that exposes the tail without publishing its payload. |
| Delivery | Two sessions share one socket slot over time. A report can be staged for its owner or dropped. Logon acknowledgement uses sequence one. | Gateway tests check sequence numbers, reconnect, and a report for a prior occupant after slot reuse. Staged output can still be lost after socket close. |
| Loadgen | The generator's tracked-order FIFO and request sequence. | A loopback peer checks 8,192 real requests, including symbol, order ID, sequence, and cancel order. |
| FaultGame | Three abstract replicas, verification ownership, and a result based on sampled commit positions. | The existing fault game test drives seven real replica processes, faults, recovery, and agreement checks. |
| libvsr | Upstream quorum, durable commit, view change, and journal recovery. | Run the pinned model, its mutation configurations, and MEX's replicated smoke test. |

The [catalog](catalog.json) has the exact bounds and CTest mappings. Each MEX
model has a deliberate incorrect behavior that must fail. Replay currently
covers matching and the replication bridge. The remaining models use the
mapped CTest tests for implementation evidence. TLA+ here abstracts byte
formats and OS buffer behavior. SpscMemory checks a bounded release and
acquire abstraction. A C++ memory-model checker for the production atomics
would provide separate implementation evidence.

## Update

When a covered file changes, check the model at that boundary and update its
configuration or replay driver if its behavior changed. The initial source
baseline is MEX 570d77ac5b41347bb5f96b42d5c78eb354637f1a. Keep the bounded
claim and production evidence separate when you report results.

Generate the KaizenFlow mapping from the catalog:

    verification/TLA/run rules > .kaizen-checks

CI checks the generated mapping, uses KaizenFlow to select affected models,
and runs all configurations on the weekly schedule. Keep a regression case
for any counterexample that can run against the implementation.
