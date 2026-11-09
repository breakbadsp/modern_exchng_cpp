---
name: cpp-hot-path
description: "C++ hot-path performance: measure first, no allocation in the steady state, limits on loops and caches, batching, contiguous data, no virtual dispatch or RTTI, likely and unlikely. Read when you change code that runs for each event at a high rate, or make a performance change."
disable-model-invocation: true
---

# The hot path

The hot path is the code that runs for each event at a high rate: each
packet, each sample, each request. The project says which code that is.

- **Measure first** (Per.1, Per.2, Per.6). Make a quick estimate of
  network, disk, memory, and CPU during design. After that, change hot code
  only with a measurement, and report the before and after numbers.
- **Do not allocate on the hot path.** Allocate and size buffers at startup
  or reload, then reuse them. Keep maps keyed by a stable key, so that they
  are not rebuilt each cycle. A project can permit other allocation points
  (for example, when a new key first appears). Use only the points that the
  project permits.
- **Put a limit on everything.** Each loop over external data has a maximum
  (items per packet, bytes per message, entries in a cache). A limit that
  is reached is an operating error: count it, log it once, and continue.
  The program must not grow without limit because an input source
  misbehaves.
- **Batch.** Write to a database in transactions, not row by row. Let the
  program run at its own pace. Do not do I/O as a direct reaction to each
  event.
- **Keep data contiguous.** Use `std::vector<Record>`, not
  `std::vector<std::unique_ptr<Record>>`, for data that a loop walks.
- **No virtual dispatch or RTTI** (`dynamic_cast`, `typeid`) in per-event
  code. Use templates or `if constexpr`.
- **`[[likely]]` and `[[unlikely]]`** only on a branch that a profile shows
  to be important.
