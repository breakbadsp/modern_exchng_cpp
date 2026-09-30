# HFT Matching Engine — Design Spec

Use this as the continuation prompt for further design work. It locks in
everything decided so far and flags what's still open, so a new session
(or another engineer) has full context without re-deriving it.

## Role & working style

Act as a senior HFT engineer/architect. Before proposing anything, point
out what's underspecified or questionable rather than silently working
around gaps. Give code sketches to clarify a point, not full
implementations, unless a step is explicitly ready to be fleshed out.
We're building this **one step at a time** — don't jump ahead of wherever
we currently are, and don't silently revisit an earlier locked decision;
surface the tension and ask first (see prior discussions on the ladder
vs. sorted-vector debate for why this matters).

## Toolchain

- Target **C++26** (`-std=c++26`). Verified current as of this writing:
  **GCC 16.2** and **Clang/LLVM 23.1** (Aug 2026) use whichever supports c++26. C++26 is only partially implemented in either compiler.
- Compiled on Arch Linux (Omarchy).

## Scope (locked)

- Equity asset class only.
- Order types: **Market and Limit only**. No multi-leg/spread orders.
- **One symbol per `Book` instance** — assumed throughout the code (no
  `symbol_id` field anywhere), but never explicitly re-confirmed in so
  many words. Flagging so it can be corrected if wrong. Multi-symbol
  routing (how multiple `Book` instances are managed/dispatched to) is
  undesigned.
- **FIFO (price-time) priority** for orders at the same price.
- **Single-threaded** matching core — no locks, no atomics, no
  concurrency in v1.
- v1 operations: **New (limit) order, Cancel, and partial fills.**
  Modify/price-replace was mentioned once in passing but never resolved
  — see Open Questions.
- Throughput target: **100k–500k+ orders/sec**, single thread.

## Hard design guidelines (locked)

1. Single-threaded, intentionally — simplicity over premature concurrency.
2. **No heap allocation once the system is running.** The order pool, free
   stack, fill buffer, and bid/ask ladders are allocated once at
   construction (`unique_ptr[]` / `PriceLadder`). Exceeding a capacity at
   runtime is an **explicit rejection**, never a silent grow. `Book` is
   neither copyable nor movable: capacity is fixed for the object's life,
   and `Fills()` / `Bids()` spans would dangle across a move.
3. Within that constraint, minimize data movement/churn where it's cheap
   — but bounded O(n) `memmove` shifts in the price ladders were a
   **conscious, accepted trade-off** for simplicity, not eliminated (see
   "Price ladder vs. sorted vector" below).

## Key architectural decision: Market orders never rest

Only **Limit** orders occupy a resting slot in the book. A Market order
is a transient aggressor — it matches immediately against whatever
liquidity exists (fully, partially, or is rejected if there's not
enough) and is never stored. Consequence: the resting-order struct needs
no order-type field at all — "being in the book" already implies "limit
order." (Market-order handling itself is part of the still-undesigned
matching algorithm.)

## Order representation (locked) — `OrderNode`, in `order.hpp` → `book.hpp`

Final fields: `price` (`int64_t`, integer **ticks** — never float/double,
for exact comparisons and cheaper compares), `quantity` (`uint32_t`,
**remaining** quantity only), `prev`/`next` (`uint32_t` pool indices —
intrusive doubly-linked list within a price level), `side` (1-byte enum).

Deliberately excluded, with reasoning:
- **`order_id`** — implicit; it *is* the struct's index in the
  preallocated order pool. Storing it again would be redundant.
- **`sequence`** — unnecessary. FIFO priority is enforced structurally:
  new orders always append at a price level's list tail, matching always
  consumes from the head. List position *is* priority — nothing in the
  hot path ever compares a sequence number. (If a pure arrival-order
  audit trail is wanted later — e.g. for compliance/replay — that would
  be a *separate* monotonic counter, decoupled from `order_id` since
  `order_id` slots get reused, used only for logging. Not implemented.)
- `symbol_id` — one `Book` = one symbol (see Scope caveat above).
- `order_type`, `status` — type is unnecessary (see above); status is
  inferable (`quantity == 0` → filled, absent from the book → cancelled
  or filled).

## Order book structure (locked) — sorted flat vectors + intrusive lists

**Considered and rejected:**
- `std::map`-based tree: correct but poor cache locality (heap-allocated,
  scattered nodes), O(log n) with a bad constant factor.
- Direct-indexed price ladder (array indexed by price offset): O(1)
  always, zero data movement — but requires committing to and bounding a
  price range up front, plus a hard-reject policy for out-of-band
  prices. Revisited once under the no-allocation guideline (since both
  approaches ended up needing an up-front bound anyway) and still
  rejected in favor of keeping the simpler structure.

**Chosen:** a fixed-capacity `PriceLadder` per side (sorted `PriceLevel`
array, size ≤ capacity, no reallocation after construct).

- `PriceLevel`: `price` (`int64_t`), `head`/`tail` (pool indices),
  `order_count`, `total_qty` (O(1) liquidity check at a level without
  walking its list).
- **Storage order convention**: BIDS ascending (best bid =
  `bids_.back()`); ASKS descending (best ask = `asks_.back()`). Both
  sides keep their most active price at the tail on purpose —
  insert/erase cost is proportional to *distance from the end*, not to
  nearest end — so concentrating the highest-churn
  region (top of book) at the tail makes the most frequent operations
  (new best-price levels, emptied best-price levels) cheap, while rarer
  activity deep in the book absorbs the full O(n) shift.
- Empty levels are **erased** (dense array), not left as placeholders —
  justified specifically by the tail-ordering trick making erasure at
  the hot spot cheap.
- `find_level`: binary search (`std::lower_bound`) with a
  side-dependent comparator (ascending for bids, descending for asks).
  Returns either the existing level's index or the correct insertion
  index; caller checks `levels[idx].price == price` to tell them apart.
- **Two distinct operations per incoming order, never conflated:**
  1. *Matching* — linear/sequential scan of the **opposite** side,
     consuming liquidity front-to-back. Must be sequential — price-time
     priority means you can't skip levels. Fills are written into a
     fixed buffer (`Fills()`), one record per distinct maker (so at most
     `max_orders` fills). Matching does not invoke user code.
  2. *Resting insertion* — for any leftover quantity, a binary-search
     lookup into the **same** side's array. No consumption semantics,
     so binary search applies cleanly. The free stack is LIFO: a maker
     slot released during matching is the first id Rest reuses. If a
     fill has `maker_removed_`, do not look `maker_id_` up on the book;
     the fill's `side_` / `price_` / `quantity_` are the maker snapshot.
- **Cancel**: `order_id` → O(1) → `pool_[order_id].price` → O(log n)
  binary search to relocate the order's *current* price-level position
  (it may have shifted since insertion) → O(1) unlink via `prev`/`next`.
  This deliberately avoids maintaining a fragile `order_id → vector
  index` map, which would go stale on every insert/erase elsewhere in
  the array.

## No-allocation implementation (locked)

- `Book(max_orders, max_price_levels)` constructor: pool, free stack, and
  fill buffer are `unique_ptr[]` of length `max_orders`; both ladders
  allocate `max_price_levels` slots (or none if that cap is 0).
- Acquiring an order slot is always a pop from the free stack — no
  growth path exists anywhere in the matching path.
- Capacity checks (`free_count_ == 0`, ladder size vs cap) decide
  whether a remainder can rest. A reject **with no fills** leaves the
  book untouched. A **partial fill that then cannot rest** keeps those
  fills and drops the leftover quantity; that is accepted, not a
  no-op reject.
- Rejection is `SubmitStatus::Rejected` (or an accepted trade that does
  not rest), not an exception.

## Deliverables so far

- `types.hpp` — `Price`, `Quantity`, `OrderNode`, `PriceLevel`, `Fill`,
  `SubmitResult`.
- `book.hpp` / `book.cpp` — `PriceLadder` and `Book`: limit and market
  submit, cancel, matching, `Fills()` after each submit.

## Explicitly open / not yet decided

- **Matching algorithm itself** — implemented for limit and market.
  Policy details above `Book` (logging, alerts) are still open.
- **Concrete `max_orders` / `max_price_levels` values** — currently just
  constructor parameters. Need sizing against the throughput target and
  the specific instrument's realistic order-flow depth and distinct
  price-level count.
- **Policy on `INVALID` rejection** — reject the client order, log,
  alert? This belongs to the layer above `Book`; not decided.
- **Modify / price-replace semantics** — mentioned once, unresolved.
  Real markets usually have a rule about whether a price change loses
  time priority (typically yes, sometimes not for quantity-decrease-only
  modifies). Out of the locked v1 scope; needs a decision if/when added.
- **Client-facing order ID vs. internal `order_id`** — not addressed. A
  client-supplied identifier (e.g. a FIX ClOrdID) would need its own
  lookup structure to map to the internal pool index, which would itself
  need pre-allocating to respect the no-allocation guideline.
- **Multi-symbol routing** — undesigned (see Scope caveat above).
- **Tick size / price scale** — `price` is `int64_t` ticks, but the
  concrete minimum increment (paise, cents, or otherwise) for the target
  instrument hasn't been pinned down.
- **Input validation** — zero quantity is a normal reject. Negative
  prices are allowed (integer ticks). Further policy (client-facing
  checks) belongs above `Book`.
- **Audit / arrival-order trail** — if a true arrival-order record
  independent of pool-slot reuse is ever needed (compliance, replay,
  debugging), that's a separate monotonic counter, not yet added.

## Where we are

Steps: (1) Order representation — **done**. (2) Order book structure —
**done**. (3) Matching — **done**. Next work is above `Book` (client ids,
modify, multi-symbol).
