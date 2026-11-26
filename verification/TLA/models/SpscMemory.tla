------------------------------ MODULE SpscMemory ------------------------------
EXTENDS Naturals

CONSTANTS Capacity, MaxItem, RelaxedTailRead
VARIABLES head, tail, cachedHead, cachedTail, visibleThrough, slots, writing, reading, item

State == <<head, tail, cachedHead, cachedTail, visibleThrough, slots, writing, reading, item>>
Slot(n) == n % Capacity
Init ==
    /\ head = 0 /\ tail = 0 /\ cachedHead = 0 /\ cachedTail = 0
    /\ visibleThrough = 0
    /\ slots = [n \in 0..(Capacity - 1) |-> 0]
    /\ writing = FALSE /\ reading = FALSE /\ item = 0

\* A stale acquire of head can retain an older cache value.
RefreshHead ==
    /\ ~writing /\ tail - cachedHead = Capacity
    /\ cachedHead' \in cachedHead..head
    /\ UNCHANGED <<head, tail, cachedTail, visibleThrough, slots, writing, reading, item>>

Write ==
    /\ ~writing /\ tail < MaxItem /\ tail - cachedHead < Capacity
    /\ slots' = [slots EXCEPT ![Slot(tail)] = tail + 1]
    /\ writing' = TRUE
    /\ UNCHANGED <<head, tail, cachedHead, cachedTail, visibleThrough, reading, item>>

PublishTail ==
    /\ writing
    /\ tail' = tail + 1
    /\ writing' = FALSE
    /\ UNCHANGED <<head, cachedHead, cachedTail, visibleThrough, slots, reading, item>>

\* An acquire that observes a release of tail also observes the preceding writes.
RefreshTail ==
    /\ ~reading /\ head = cachedTail
    /\ cachedTail' \in cachedTail..tail
    /\ visibleThrough' = IF RelaxedTailRead THEN visibleThrough ELSE cachedTail'
    /\ UNCHANGED <<head, tail, cachedHead, slots, writing, reading, item>>

Read ==
    /\ ~reading /\ head < cachedTail
    /\ item' = IF head < visibleThrough THEN slots[Slot(head)] ELSE 0
    /\ reading' = TRUE
    /\ UNCHANGED <<head, tail, cachedHead, cachedTail, visibleThrough, slots, writing>>

PublishHead ==
    /\ reading
    /\ head' = head + 1
    /\ reading' = FALSE
    /\ UNCHANGED <<tail, cachedHead, cachedTail, visibleThrough, slots, writing, item>>

Next == RefreshHead \/ Write \/ PublishTail \/ RefreshTail \/ Read \/ PublishHead \/ UNCHANGED State

ValidPrefix ==
    /\ cachedHead <= head /\ head <= cachedTail /\ cachedTail <= tail
    /\ tail - head <= Capacity
    /\ writing => tail - head < Capacity

ReadPublishedItem == reading => item = head + 1
ReadingSlotOwned == reading => slots[Slot(head)] = item
===============================================================================
