# HFT Matching Engine — I/O, Queue, and Engine Design Spec

Continuation of [`design-spec.md`](design-spec.md). That spec locks the
single-symbol `Book`. This one covers everything around it: accepting
client connections, receiving orders, routing them to the right `Book`,
and returning responses.

Same working style as `design-spec.md`: build one step at a time, and
surface tension with a locked decision instead of silently revisiting it.

**Status:** the v1 decisions below were **locked** on 2026-11-07. Items
under "Still open" are not decided yet. On 2026-11-10 the
[replication preparation](#replication-preparation-locked) changed some of
them; each changed decision is marked "Changed 2026-11-10" with its reason.

## Guiding principle (locked)

Each `Book` stays single-threaded. All concurrency is confined to SPSC
queues; each queue has exactly one producer thread and one consumer
thread. No locks, no shared mutable state outside the queues. The
symbol routing table and the session list are written once at startup
and are read-only after that.

`Book` gets two new operations for Replace (see "Replace"). Everything
else above matching lives in the Engine.

## What exists vs. what is to build

| Piece | State |
|---|---|
| `Book` (matching, resting, cancel, fills) | Done — `src/book.hpp` |
| `Book` replace operations (`ReduceOrder`, `ReplaceOrder`) | Done — `src/book.hpp` |
| SPSC queue | Done — `src/spsc_queue.hpp` |
| Wire protocol (frame structs, encode/decode) | Done — `src/protocol.hpp` |
| Config file parser | Done — `src/config.hpp` |
| Engine (thread loop around `Book`s, order metadata, reports) | Done — `src/engine.hpp` |
| Gateway (sockets, sessions, framing, routing, writes) | Done — `src/gateway.hpp` |
| `main` + load-generator client | Done — `src/main.cpp`, `tools/loadgen.cpp` |
| Replication preparation (request and op numbers, `Engine::Apply`, state digest) | Done — `src/engine.hpp` |
| Replication (VSR replicas, log, view change, journal) | Done with libvsr — `src/replication.hpp`, [docs/replication.md](../docs/replication.md) |

`Book` is the matching logic only. It does not own a thread, a queue,
client identities, or outbound messages. The Engine is the thin layer that
adds those.

## Architecture (locked)

```
            clients (TCP)
               │  ▲
               ▼  │
   ┌─────────────────────────────┐
   │  Gateway thread (1)         │  epoll, accept, logon, framing,
   │  - connections[]            │  validation, symbol → engine routing,
   │  - sessions[]               │  socket writes
   └─────────────────────────────┘
      │ SPSC in            ▲ SPSC out        (one pair per engine)
      ▼                    │
   ┌──────────┐  ┌──────────┐  ┌──────────┐
   │ Engine 0 │  │ Engine 1 │  │ Engine 2 │  each: pinned thread, busy-poll,
   │ Book AAPL│  │ Book MSFT│  │ Book X,Y │  owns its Books exclusively
   └──────────┘  └──────────┘  └──────────┘
```

### Components

1. **Gateway** (one thread in v1)
   - Owns every socket; the only thread making network syscalls.
   - Accepts connections, checks logon, reads bytes, reassembles frames,
     validates fields.
   - Maps `symbol_id → (engine, symbol_slot)` and pushes a command onto
     that engine's inbound queue. Every command carries the `SessionId`
     and an op number (see "Replication preparation").
   - Drains every engine's outbound queue and writes each report to the
     connection of its session.
   - Holds session state (logon, outbound sequence number) and **no order
     state**.
2. **Engine** (N threads)
   - Owns one or more `Book`s plus per-order metadata and a client-id
     index for each.
   - Pops a command and applies it: checks the request number, calls
     `Book`, and converts `SubmitResult` + `Fills()` into outbound reports.
   - Keeps the client table (the last applied request number for each
     session) and the last applied op number.
   - No syscalls, no locks, no allocation after startup.
3. **SPSC queues**
   - Fixed power-of-two capacity, allocated at startup.
   - Fixed-size POD messages.
   - Head and tail on separate cache lines; each side caches the other's
     index to cut cache-line traffic; acquire/release ordering.
   - The only memory shared between threads.

## Threading: engine shards (locked)

An engine thread owns a *set* of books, with a fixed `symbol → engine`
map from startup config. One thread per symbol does not scale: a
busy-polling thread needs its own core, so the symbol count would be
capped at the core count. Configured as one symbol per engine, the shard
model is exactly thread-per-symbol. Cold symbols can share an engine and
hot symbols keep their own, with no design change. The overhead is one
array index (`books_[symbol_slot]`).

Each engine thread constructs its own `Book`s at startup, so the memory
is first touched by the thread that uses it (NUMA-local).

Ordering guarantees that follow:

- Per symbol: total order. One gateway and one FIFO queue per engine, so
  arrival order at the gateway is priority order in the book.
- Per client across symbols: none. Acceptable.
- Across engines: the op number gives one total order of the accepted
  commands (Changed 2026-11-10; a replicated log needs it). The engines
  still apply their commands in parallel.

Threads are pinned and busy-poll. A backoff (pause → yield) mode is useful
for development machines so idle engines do not burn cores.

One gateway thread in v1. M gateways later means M inbound and M
outbound queues per engine, and fairness across gateways must then be
defined.

## Sessions and logon (locked)

- The first frame on a connection must be
  `Logon{session_name, token}`. The gateway checks it against the static
  session list from config and replies `LogonAck` or `LogonReject`, then
  closes the connection on reject. Any other frame before logon closes the
  connection.
- A session has a fixed `SessionId` (its index in the session list). Only
  one connection per session at a time: a second logon for an active
  session is rejected.
- **The session, not the connection, owns orders.** After a reconnect and
  a new logon, the client can cancel and replace its resting orders.
- The gateway keeps `sessions[session_id] → current connection`. A report
  for a session that has no connection now is dropped.
- No TLS in v1. The token is a shared secret in plain text; this is for a
  trusted network only.

## Wire protocol (locked)

Binary, fixed-size, little-endian. Header `{u16 length; u8 type;}` then a
POD body. No FIX in v1; a FIX translator can sit in the gateway later.
Symbols are `u32` ids from the startup config, never strings on the
wire.

| Client → Exchange | Exchange → Client |
|---|---|
| `Logon{session_name, token}` | `LogonAck` / `LogonReject{reason}` |
| `NewOrder{request_number u64, client_order_id u64, symbol_id u32, side, type, price i64, qty u32}` | `Ack{client_order_id, symbol_id, exchange_order_id, leaves}` |
| `Cancel{request_number, client_order_id, symbol_id}` | `Reject{client_order_id, reason}` |
| `Replace{request_number, client_order_id, symbol_id, new_price i64, new_total_qty u32}` | `Fill{exchange_order_id, client_order_id, price, qty, leaves, liquidity}` |
| | `CancelAck` / `CancelReject{reason}` |
| | `ReplaceAck{client_order_id, price, leaves}` / `ReplaceReject{reason}` |
| | `Done{client_order_id, reason}` — leaves dropped |

- Every exchange → client frame carries a `u64 seq`. It starts at 1 on
  each logon and increases by 1 for each frame to that session. It lets
  the client detect a gap. There is **no resend** in v1.
- Every order report (`Ack`, `Reject`, `Fill`, `CancelAck`,
  `CancelReject`, `ReplaceAck`, `ReplaceReject`, `Done`) also carries
  `symbol_id`. A `client_order_id` is unique only per session per
  symbol, so without it a report can be ambiguous.
- `exchange_order_id` is informational in v1. Cancel and Replace use
  `client_order_id` (see below).
- Changed 2026-11-10: `NewOrder`, `Cancel`, and `Replace` start with a
  `u64 request_number`. Reason: replication must not execute a retried
  request twice, and `client_order_id` cannot show that, because it is free
  again when its order is filled or cancelled. The rules are in
  "Replication preparation".

### Gateway validation (locked)

Before anything reaches an engine: session logged on, known message type,
exact length, known `symbol_id`, valid side and type, `qty > 0`, and for a
limit order `min_price ≤ price ≤ max_price` from that symbol's config. The
same price check applies to `Replace.new_price`. Dynamic bands (for
example, distance from the last trade) are out of v1 scope.

All checks that need order state happen in the engine.

## Order ids and ownership (locked)

`Book`'s `OrderId` is a pool slot that is **reused**. It never goes on the
wire as a key.

Each engine keeps, per book, a metadata array sized `max_orders` and a
client-id index, both allocated at startup:

```cpp
struct OrderMeta
{
  SessionId owner_{};
  std::uint64_t client_order_id_ = 0;
  Quantity cum_qty_{};           // filled so far; needed for Replace total qty
  std::uint32_t generation_ = 0; // bumped each time the slot is claimed
};
// exchange_order_id = (u64{generation} << 32) | slot

// Open addressing, linear probing, backward-shift delete (no tombstones).
// Capacity: power of two, at least 2 × max_orders.
// Key: (owner, client_order_id). Value: OrderId (pool slot).
class ClientOrderIndex;
```

- **Key scope.** `client_order_id` is unique per session per symbol, and
  only among **live** (resting) orders. Each book has its own index.
- **Duplicate.** A `NewOrder` whose key is already live is rejected with
  `Reject{DuplicateClientOrderId}` before `Book` is called. An id of a
  filled, canceled, or done order may be reused. This rule is about order
  ids. A repeated request is found by its request number, before this check.
- **Why the engine owns the index.** New, Cancel, and Replace for one
  order go through the same gateway and the same queue, in order. So a
  Cancel sent right after a New always finds the order, even before the
  client sees the `Ack`. If the gateway kept the index, it would not know
  the slot until the `Ack` came back.
- **Cancel / Replace lookup.** Find `(session, client_order_id)` in the
  index. Not found → `CancelReject` / `ReplaceReject{UnknownOrder}`. The
  index only holds live orders, so a stale key cannot reach a reused slot.
- **Index upkeep.** Insert when an order rests. Erase when it is canceled
  or fully filled as a maker (`Fill::maker_removed_`). An order that never
  rests is never inserted.
- **Maker before claim.** Within one submit, `Rest` may reuse a slot that
  matching just freed (the free stack is LIFO). The engine must therefore
  process every fill from `Fills()` (report to the maker, add to its
  `cum_qty_`, erase it from the index if removed) *before* it writes
  `meta[r.order_id_]` and inserts the new order. This needs a dedicated
  test.

```cpp
void Engine::OnNewOrder(const NewOrderCmd &p_cmd)
{
  Instrument &inst = instruments_[p_cmd.symbol_slot_];
  if (inst.index_.Contains(p_cmd.owner_, p_cmd.client_order_id_))
  {
    EmitReject(p_cmd, RejectReason::DuplicateClientOrderId);
    return;
  }
  const SubmitResult r = /* SubmitLimitOrder or SubmitMarketOrder */;
  for (const Fill &fill : inst.book_.Fills()) // 1. makers, while meta is still theirs
  {
    inst.OnMakerFill(fill); // report, cum_qty_, erase from index if removed
    EmitFill(p_cmd.owner_, fill, Liquidity::Taker);
  }
  if (r.order_id_ != kInvalidOrderId) // 2. only now claim the slot
  {
    inst.Claim(r.order_id_, p_cmd); // meta, generation, index insert
  }
  EmitFinal(p_cmd, r); // 3. Ack(leaves), Reject, or Done
}
```

## Replace (locked)

`Replace{client_order_id, symbol_id, new_price, new_total_qty}`. The order
keeps its `client_order_id`.

- `new_total_qty` is the total order quantity, filled part included (FIX
  style). `new_leaves = new_total_qty − cum_qty_`.
- `new_total_qty ≤ cum_qty_` → `ReplaceReject{TooLate}`. Use Cancel to
  remove an order.
- **Same price and `new_leaves < leaves`:** reduce in place. The order
  **keeps its time priority**. `Book::ReduceOrder(order_id, new_leaves)`
  updates the order and its level's `total_qty_`.
- **Price change, or `new_leaves ≥ leaves`:** the order **loses priority**.
  `Book::ReplaceOrder(order_id, new_price, new_leaves)` removes it and
  submits the leaves as a new limit order at `new_price`. It can match at
  once; the engine handles its fills the same way as for a `NewOrder`
  (maker before claim).
- **No partial change on reject.** `Book::ReplaceOrder` checks before it
  changes anything. If `new_price` needs a new level on that side and the
  ladder has no room even after the order leaves its own level, it rejects
  and the original order stays as it was. This check can reject a replace
  that would have matched fully. The pool cannot be full here, because the
  removed order frees its own slot.
- A replaced order that rests in a new slot keeps the same
  `client_order_id`, `owner_`, and `cum_qty_`; the index entry moves to
  the new slot. The new slot has a new `generation_`, so the
  `exchange_order_id` in later reports changes.

## Dropped leaves (locked)

A partial fill whose remainder cannot rest (pool or ladder full), and the
unfilled part of a market order, are dropped by `Book`. The engine
reports them as `Done{client_order_id, reason}` after the fills.

## Backpressure (locked, deadlock-free by construction)

| Full queue / buffer | Policy |
|---|---|
| Gateway → engine inbound | Reject immediately with `Reject{SystemBusy}`. Gateway **never blocks**. |
| Engine → gateway outbound | Engine **spins** until space and counts the stall. Safe because the gateway never blocks, so it always drains. |
| Per-connection send buffer (fixed size) | **Disconnect** that client. One slow consumer must not stall others. |

Rules 1 and 2 together break the only possible wait cycle. If both
sides could block, gateway and engine could deadlock on each other.

## No-allocation compliance (locked)

Allocated once at startup, never grown:

- Connection table (`max_connections`), each with fixed read and send
  buffers.
- Session table (from config).
- All SPSC rings.
- Each engine's `Book`s, their `OrderMeta` arrays, and their
  `ClientOrderIndex` tables.
- Each engine's client table: one request number for each configured
  session.

Exceeding any of them is an explicit reject or disconnect, consistent with
`design-spec.md`.

## Configuration (locked)

One plain-text, line-based file, read once at startup by a small parser
of our own (no new dependency): `ParseConfig(std::string_view)` and
`LoadConfig(path)` in `src/config.hpp`. One record per line, fields
separated by blanks. A line that starts with `#` is a comment; blank
lines are ignored. `config/example.cfg` is a full example.

```
engine <core>
symbol <symbol_id> <engine> <max_orders> <max_price_levels> <min_price> <max_price>
session <session_name> <token>
gateway address <ipv4>
gateway port <port>
gateway max_connections <n>
gateway inbound_queue_capacity <n>
gateway outbound_queue_capacity <n>
```

- The engine index and the `SessionId` are the order of their lines,
  from 0. A symbol names an engine declared above it. A symbol's slot in
  its engine is its order among that engine's symbols.
- Numbers are whole decimal tokens. Prices are ticks.
- Rejected: unknown keyword or gateway key, wrong field count, bad or
  out-of-range number, duplicate `symbol_id`, session name, or gateway
  key, `min_price > max_price`, `max_orders` or port 0, a name or token
  over 16 bytes, a queue capacity that is not a power of two, a non-IPv4
  address, an engine with no symbols, a missing gateway key, and no
  engines, symbols, or sessions.

A bad config stops startup with an error that names the line (0 when the
error is not on one line) and the reason. Nothing is read after startup.

## Out of v1 scope (locked)

Stated so each one is a decision, not an accident:

- **Cancel-on-disconnect.** Orders stay resting when a session drops.
  Adding it needs a per-session intrusive list of order slots in each
  engine.
- **Journal / recovery.** One process without replication loses the book
  in a crash. A replicated cluster keeps it (`docs/replication.md`).
- **Resend / replay** of exchange → client frames after reconnect.
- **Replication.** Done with libvsr; see `docs/replication.md`.
- **Market data feed.** Would be an extra outbound queue per engine.
- **FIX**, **TLS**, **multiple gateways**, **kernel bypass**. Transport is
  kernel TCP with non-blocking sockets and level-triggered epoll.

## Replication preparation (locked)

Locked on 2026-11-10. A Viewstamped Replication (VSR) library will later
make several copies of the exchange agree on one ordered sequence of
commands. Nothing here replicates. These are the fields, the state, and the
seams that replication needs, and the single-process exchange works as
before.

The replicated state machine is the `Engine`. Its state and its reports
depend only on its configuration and on the commands it applied, in order.
All replicas must use the same configuration: book capacities, symbol
slots, and the session list.

### Request identity

- `NewOrder`, `Cancel`, and `Replace` carry a `request_number`
  (`wire::RequestNumber`). The session, not the connection, owns the
  sequence. The client starts at 1 and uses a higher number for each new
  request, also after a reconnect. A retry repeats the request unchanged,
  with its number.
- Each engine keeps a **client table**: for each session, the last request
  number that this engine applied. The table is in the engine because it
  is replicated state: every replica must make the same decision about a
  retry. The session list is fixed, so the table has a fixed size and no
  eviction.
- A session can send to several engines. The symbol selects the engine, so
  a retry reaches the engine that got the first attempt. One engine sees
  increasing numbers with gaps.
- **Duplicate outcome.** A request whose number is not above the last
  applied number of its session at that engine is not executed. It gets
  exactly one report: the reject of its type (`Reject`, `CancelReject`, or
  `ReplaceReject`) with reason `DuplicateRequest`. The client table and the
  books do not change. This covers a retry of an applied request, a number
  sent out of order, and number 0.
- Why a reject: each request keeps exactly one final report, the engine
  stores nothing more than one number for each session, and the client
  gets an explicit answer. The reject says that the engine did not execute
  the request now. It does not carry the result of the first attempt.
- A request that the gateway rejects (`UnknownSymbol`, `InvalidQuantity`,
  `PriceOutOfRange`, `SystemBusy`) does not reach an engine and does not
  use its number there. If later requests of the session were applied by
  that engine, the client sends the rejected request again with a new,
  higher number.
- **Not done: resend of the earlier reply.** It needs a `request_number`
  in every report, and a store of the reports of earlier requests in the
  replicated state of the engine. A request has a variable number of
  reports (fills), and clients send many requests before the first reply,
  so the store must hold a window of requests for each session, with a
  fixed size and a rule for a retry older than the window. The fills of
  the makers go to other sessions; they belong to "Resend of reports"
  below.

### Operation number

- Every `Command` carries an `OpNumber`: its position in the one total
  order over all engines. The first command is 1.
- For now the gateway assigns it from one counter, when it pushes the
  command to an engine. A command that is not queued (`SystemBusy`) takes
  no number. This counter stands in for the replicated log.
- Each engine records the last op it applied. `Engine::Apply` has the
  precondition that the op number is above it. One engine sees gaps,
  because other engines apply the ops between.
- `Command` grew from 48 to 64 bytes: 8 for the request number in each
  order and 8 for the op number.

### Apply seam

`Engine::Apply(const Command &)` applies one command to the state machine
and pushes its reports. `Engine::ProcessOne` is a pop from the inbound queue
plus `Apply`. A later caller can drive the engine from a committed log
entry with `Apply`. There is no other new abstraction.

### State digest

`Engine::StateDigest()` returns a 64-bit digest of all replicated state, in
this order: the last applied op; the client table by session id; then for
each book by symbol slot: its symbol id; the bids and then the asks, each
level in ladder order (price, order count) and each order in time priority
(slot, quantity, owner, client_order_id, filled quantity, and the slot that
the client-order index gives for its key); the size of the index; the
generation of every slot; and the free slots in stack order.

- Slot numbers, generations, and the order of the free slots are part of
  the state, because `exchange_order_id` shows them to clients.
- The digest does not use addresses or the probe layout of the index.
- It is not a cryptographic hash. It finds divergence; it does not protect
  against a hostile replica.

`tests/engine_test.cpp` (`EngineDeterminismTest`) feeds one generated
command sequence to two independent engines, one through its queue and one
through `Apply`, and requires equal reports and equal digests after every
command.

### Still out of scope, and what each needs

- **Snapshot and restore: done.** `src/engine.hpp` and `src/engine.cpp`
  provide `SnapshotBytesMax`, `Snapshot`, and `Restore`. `Book::Restore`
  in `src/book.hpp` and `src/book.cpp` restores slots and free-slot order.
  The engine builds the client-order index again. Tests are in
  `tests/engine_test.cpp`. With several engines, stop all engines at one
  op number before you take an exchange snapshot.
- **Resend of reports after reconnect.** A report sequence that belongs to
  the session and continues across logons, a fixed store of sent reports
  for each session, and a client message that asks for the reports from a
  sequence number. The `seq` of today cannot be used: the gateway assigns
  it in the order in which it drains the engines, which depends on thread
  timing when a session trades on more than one engine. A replicated
  sequence must come from the engines.
- **Report sink for backups: done.** `Engine::SetReportsEnabled(false)`
  makes `Apply` change state and push no report. Each replica pushes the
  reports of live operations to its own gateway (`docs/replication.md`).

## Still open

1. **Per-symbol sizing values.** A 1M-order pool is ~24 MB, plus
   `OrderMeta` and the index, per book. The values go in config; concrete
   numbers per instrument are not chosen yet.
2. **Tick size / price scale** for the target instrument (see
   `design-spec.md`).

## Build order

Each step is testable on its own:

1. `spsc_queue.hpp` — ring buffer plus a two-thread stress test.
2. `protocol.hpp` — wire structs, encode/decode, partial-frame handling;
   unit tests without sockets.
3. `Book::ReduceOrder` / `Book::ReplaceOrder` — tests for priority kept
   and lost, and the shadow book extended to replace.
4. `engine.hpp/.cpp` — driven through its queues in tests, no network.
   Covers the client-id index, duplicate reject, ownership,
   maker-before-claim, Replace total-qty math, and `Done`.
5. Config parser — unit tests with good and bad files.
6. `gateway.hpp/.cpp` — epoll, logon, connection and session tables,
   routing, writes.
7. `main.cpp` + a load-generator client for end-to-end throughput against
   the 100k–500k ops/s target.

## Where we are

v1 design locked. Steps 1 (SPSC queue), 2 (`protocol.hpp`), 3
(`Book::ReduceOrder` / `Book::ReplaceOrder`), 4 (`engine.hpp/.cpp`),
5 (`config.hpp/.cpp`), 6 (`gateway.hpp/.cpp`), and 7 (`main` and the load
generator) are done: all v1 steps are built. The measured end-to-end result
is in [`docs/performance.md`](../docs/performance.md).

Choices made in step 7:

- Each engine thread pins itself to its configured core and then builds its
  `Engine`, so its `Book`s are first touched there. A pin failure is a
  warning, so the binary runs on small machines.
- The gateway runs on its own thread and is not pinned: the config has no
  gateway core. The main thread waits for SIGINT or SIGTERM.
- Shutdown stops and joins the engines first, then the gateway, because an
  engine can spin on a full outbound queue that only the gateway drains.

Gateway choices made in step 6:

- Each connection has a read buffer of 64 and a send buffer of 1024
  maximum-size frames. When a frame does not fit, the gateway first
  writes to the socket, so the send buffer only overflows when the kernel
  socket buffer is also full; then the client is disconnected.
- LogonAck and LogonReject carry seq 1. A Logon on a connection that is
  already logged on closes it, as any frame out of order does.
- A bad side or order type is a malformed frame (the decoder rejects it),
  so it closes the connection instead of sending a Reject.
- The gateway busy-polls (`epoll_wait` timeout 0), like the engines.
