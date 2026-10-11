# HFT Matching Engine — I/O, Queue, and Engine Design Spec

Continuation of [`design-spec.md`](design-spec.md). That spec locks the
single-symbol `Book`. This one covers everything around it: accepting
client connections, receiving orders, routing them to the right `Book`,
and returning responses.

Same working style as `design-spec.md`: build one step at a time, and
surface tension with a locked decision instead of silently revisiting it.
Everything below is **proposed** unless marked **locked**. Lock items as
each piece is worked through in depth.

## Guiding principle

`Book` stays single-threaded and unchanged. All concurrency is confined to
SPSC queues; each queue has exactly one producer thread and one consumer
thread. No locks, no shared mutable state outside the queues.

## What exists vs. what is to build

| Piece | State |
|---|---|
| `Book` (matching, resting, cancel, fills) | Done — `src/book.hpp` |
| SPSC queue | To build |
| Wire protocol (frame structs, encode/decode) | To build |
| Engine (thread loop around `Book`s, order metadata, reports) | To build |
| Gateway (sockets, framing, routing, writes) | To build |
| `main` + load-generator client | To build |

`Book` is the matching logic only. It does not own a thread, a queue,
client identities, or outbound messages. The Engine is the thin layer that
adds those.

## Architecture

```
            clients (TCP)
               │  ▲
               ▼  │
   ┌─────────────────────────────┐
   │  Gateway thread (1)         │  epoll, accept, framing, validation,
   │  - connections[]            │  symbol → engine routing, socket writes
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
   - Accepts connections, reads bytes, reassembles frames, validates fields.
   - Maps `symbol_id → (engine, symbol_slot)` and pushes a command onto
     that engine's inbound queue.
   - Drains every engine's outbound queue and writes to the target
     connection's send buffer.
   - Holds **no order state**.
2. **Engine** (N threads)
   - Owns one or more `Book`s plus per-order metadata for each.
   - Pops a command, calls `Book`, converts `SubmitResult` + `Fills()` into
     outbound reports.
   - No syscalls, no locks, no allocation after startup.
3. **SPSC queues**
   - Fixed power-of-two capacity, allocated at startup.
   - Fixed-size POD messages.
   - Head and tail on separate cache lines; each side caches the other's
     index to cut cache-line traffic; acquire/release ordering.
   - The only memory shared between threads.

## Threading: engine shards, not strictly one thread per symbol

One thread per symbol gives isolation, natural per-symbol ordering, and an
untouched `Book`. The cost: a busy-polling thread needs its own core, which
caps the symbol count at the core count.

**Proposed:** an engine thread owns a *set* of books, with a fixed
`symbol → engine` map from startup config. Configured as one symbol per
engine, this is exactly thread-per-symbol. Later, cold symbols can share an
engine and hot symbols keep their own, with no design change. The
overhead is one array index (`books_[symbol_slot]`).

Ordering guarantees that follow:

- Per symbol: total order (one engine, FIFO queue).
- Per client across symbols: none. Acceptable.

Threads are pinned and busy-poll. A backoff (pause → yield) mode is useful
for development machines so idle engines do not burn cores.

## Wire protocol (v1)

Binary, fixed-size, little-endian. Header `{u16 length; u8 type;}` then a
POD body. No FIX in v1; a FIX translator can sit in the gateway later.
Symbols are `u32` ids from a startup symbol table, never strings on the
wire.

| Client → Exchange | Exchange → Client |
|---|---|
| `NewOrder{client_order_id u64, symbol_id u32, side, type, price i64, qty u32}` | `Ack{client_order_id, exchange_order_id, leaves}` |
| `Cancel{client_order_id, symbol_id, exchange_order_id u64}` | `Reject{client_order_id, reason}` |
| | `Fill{exchange_order_id, client_order_id, price, qty, leaves, liquidity}` |
| | `CancelAck` / `CancelReject` |
| | `Done{client_order_id, reason}` — leaves dropped |

Gateway validation (before anything reaches an engine): known message
type, exact length, known `symbol_id`, `qty > 0`, valid side/type. Price
bands and other risk checks are out of v1 scope.

## Order ids and ownership

`Book`'s `OrderId` is a pool slot that is **reused**. Exposing it raw would
let a stale cancel hit another client's order.

Each engine keeps, per book, a metadata array sized `max_orders` and
allocated at startup:

```cpp
struct OrderMeta
{
  ConnHandle owner_{};           // connection slot + connection generation
  std::uint64_t client_order_id_ = 0;
  std::uint32_t generation_ = 0; // bumped each time the slot is claimed
};
// exchange_order_id = (u64{generation} << 32) | slot
```

- **Cancel:** split the id into slot and generation. Call
  `Book::CancelOrder` only if the slot is in range, `meta[slot].generation_`
  matches, and `meta[slot].owner_` is the requesting connection. Otherwise
  `CancelReject`. All O(1), no hash map.
- **Maker before claim.** Within one submit, `Rest` may reuse a slot that
  matching just freed. The engine must therefore emit maker reports from
  `Fills()` (reading `meta[maker_id]`) *before* writing `meta[r.order_id_]`
  for the newly resting order. This needs a dedicated test.

```cpp
void Engine::OnNewOrder(const NewOrderCmd &p_cmd)
{
  Book &book = *books_[p_cmd.symbol_slot_];
  const SubmitResult r = /* SubmitLimitOrder or SubmitMarketOrder */;
  for (const Fill &fill : book.Fills()) // 1. makers, while meta is still theirs
  {
    EmitFill(MetaOf(p_cmd.symbol_slot_, fill.maker_id_), fill, Liquidity::Maker);
    EmitFill(p_cmd.owner_, fill, Liquidity::Taker);
  }
  if (r.order_id_ != kInvalidOrderId) // 2. only now claim the slot
  {
    ClaimMeta(p_cmd.symbol_slot_, r.order_id_, p_cmd);
  }
  EmitFinal(p_cmd, r); // 3. Ack(leaves), Reject, or Done
}
```

Connections use the same slot + generation scheme (`ConnHandle`). The
gateway drops a report whose handle generation no longer matches, so a
report for a closed connection never reaches whoever now holds that slot.

## Backpressure (deadlock-free by construction)

| Full queue / buffer | Policy |
|---|---|
| Gateway → engine inbound | Reject immediately with `Reject{SystemBusy}`. Gateway **never blocks**. |
| Engine → gateway outbound | Engine **spins** until space. Safe because the gateway never blocks, so it always drains. |
| Per-connection send buffer (fixed size) | **Disconnect** that client. One slow consumer must not stall others. |

Rules 1 and 2 together break the only possible wait cycle. If both
sides could block, gateway and engine could deadlock on each other.

## No-allocation compliance

Allocated once at startup, never grown:

- Connection table (`max_connections`), each with fixed read and send
  buffers.
- All SPSC rings.
- Each engine's `Book`s and their `OrderMeta` arrays.

Exceeding any of them is an explicit reject or disconnect, consistent with
`design-spec.md`.

## Open items (decide while diving into each piece)

1. **Shards vs. strict thread-per-symbol.** Proposed: shards, configurable
   down to one symbol per engine.
2. **Dropped leaves.** A partial fill whose remainder cannot rest (pool or
   ladder full), and the unfilled part of a market order, are dropped by
   `Book`. Proposed: report as `Done{reason}`.
3. **Cancel key.** Proposed: `exchange_order_id` only in v1. Cancel by
   `client_order_id` needs a preallocated per-connection hash map.
4. **Cancel-on-disconnect.** Proposed: not in v1; orders stay resting.
   Adding it needs a per-connection intrusive list of order slots.
5. **Journal / recovery.** None in v1; a crash loses the book. Stated so it
   is a decision, not an accident.
6. **Per-symbol sizing.** A 1M-order pool is ~24 MB plus ~16 MB `OrderMeta`
   per book. Proposed: `max_orders` / `max_price_levels` per symbol in
   config.
7. **Gateway count.** One in v1. M gateways later means M inbound and M
   outbound queues per engine (engine polls its inbound queues round-robin);
   the structure is unchanged.
8. **Transport.** Kernel TCP with non-blocking sockets and epoll in v1.
   Kernel bypass is out of scope.
9. **Market data feed.** Out of scope; would be an extra outbound queue per
   engine.
10. **Authentication / sessions.** None in v1; a TCP connection is the
    identity.

## Build order

Each step is testable on its own:

1. `spsc_queue.hpp` — ring buffer plus a two-thread stress test.
2. `protocol.hpp` — wire structs, encode/decode, partial-frame handling;
   unit tests without sockets.
3. `engine.hpp/.cpp` — driven through its queues in tests, no network.
   Covers id generation, ownership checks, and maker-before-claim.
4. `gateway.hpp/.cpp` — epoll, connection table, routing, writes.
5. `main.cpp` + a load-generator client for end-to-end throughput against
   the 100k–500k ops/s target.

## Where we are

Design proposed. Nothing in this document is implemented yet. Next: step 1
(SPSC queue).
