# HFT Matching Engine — I/O, Queue, and Engine Design Spec

Continuation of [`design-spec.md`](design-spec.md). That spec locks the
single-symbol `Book`. This one covers everything around it: accepting
client connections, receiving orders, routing them to the right `Book`,
and returning responses.

Same working style as `design-spec.md`: build one step at a time, and
surface tension with a locked decision instead of silently revisiting it.

**Status:** the v1 decisions below were **locked** on 2026-11-07. Items
under "Still open" are not decided yet.

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
| `Book` replace operations (`ReduceOrder`, `ReplaceOrder`) | To build |
| SPSC queue | Done — `src/spsc_queue.hpp` |
| Wire protocol (frame structs, encode/decode) | To build |
| Config file parser | To build |
| Engine (thread loop around `Book`s, order metadata, reports) | To build |
| Gateway (sockets, sessions, framing, routing, writes) | To build |
| `main` + load-generator client | To build |

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
     that engine's inbound queue. Every command carries the `SessionId`.
   - Drains every engine's outbound queue and writes each report to the
     connection of its session.
   - Holds session state (logon, outbound sequence number) and **no order
     state**.
2. **Engine** (N threads)
   - Owns one or more `Book`s plus per-order metadata and a client-id
     index for each.
   - Pops a command, calls `Book`, converts `SubmitResult` + `Fills()` into
     outbound reports.
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
| `NewOrder{client_order_id u64, symbol_id u32, side, type, price i64, qty u32}` | `Ack{client_order_id, exchange_order_id, leaves}` |
| `Cancel{client_order_id, symbol_id}` | `Reject{client_order_id, reason}` |
| `Replace{client_order_id, symbol_id, new_price i64, new_total_qty u32}` | `Fill{exchange_order_id, client_order_id, price, qty, leaves, liquidity}` |
| | `CancelAck` / `CancelReject{reason}` |
| | `ReplaceAck{client_order_id, price, leaves}` / `ReplaceReject{reason}` |
| | `Done{client_order_id, reason}` — leaves dropped |

- Every exchange → client frame carries a `u64 seq`. It starts at 1 on
  each logon and increases by 1 for each frame to that session. It lets
  the client detect a gap. There is **no resend** in v1.
- `exchange_order_id` is informational in v1. Cancel and Replace use
  `client_order_id` (see below).

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
  filled, canceled, or done order may be reused.
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
  changes anything. If `new_price` is not an existing level on that side
  and the ladder is full, it rejects and the original order stays as it
  was. This check is conservative: it can reject a replace that would have
  matched fully. The pool cannot be full here, because the removed order
  frees its own slot.
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

Exceeding any of them is an explicit reject or disconnect, consistent with
`design-spec.md`.

## Configuration (locked)

One plain-text, line-based file, read once at startup by a small parser
of our own (no new dependency). It holds:

- Engines: count and the core to pin each one to.
- Symbols: `symbol_id`, engine index, `max_orders`, `max_price_levels`,
  `min_price`, `max_price` (ticks).
- Sessions: `session_name`, `token`.
- Gateway: listen address and port, `max_connections`, queue capacities.

A bad config stops startup with a clear error. Nothing is read after
startup.

## Out of v1 scope (locked)

Stated so each one is a decision, not an accident:

- **Cancel-on-disconnect.** Orders stay resting when a session drops.
  Adding it needs a per-session intrusive list of order slots in each
  engine.
- **Journal / recovery.** A crash loses the book.
- **Resend / replay** of exchange → client frames after reconnect.
- **Market data feed.** Would be an extra outbound queue per engine.
- **FIX**, **TLS**, **multiple gateways**, **kernel bypass**. Transport is
  kernel TCP with non-blocking sockets and level-triggered epoll.

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

v1 design locked. Step 1 (SPSC queue) is done. Next: step 2
(`protocol.hpp`).
