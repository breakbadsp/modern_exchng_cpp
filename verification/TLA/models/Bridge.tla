-------------------------------- MODULE Bridge --------------------------------
EXTENDS Naturals, FiniteSets, Sequences

CONSTANTS MaxOrder, Capacity, Mutate
VARIABLES queued, pending, retired, abandoned, issued, event

State == <<queued, pending, retired, abandoned, issued, event>>
Elements(xs) == {xs[i] : i \in 1..Len(xs)}

Init ==
    /\ queued = <<>>
    /\ pending = <<>>
    /\ retired = {}
    /\ abandoned = {}
    /\ issued = 0
    /\ event = "idle"

Enqueue ==
    /\ issued < MaxOrder
    /\ issued' = issued + 1
    /\ queued' = Append(queued, issued')
    /\ event' = "enqueue"
    /\ UNCHANGED <<pending, retired, abandoned>>

Send ==
    /\ Len(queued) > 0
    /\ Len(pending) = 0 \/ Mutate
    /\ LET n == IF Len(queued) < Capacity THEN Len(queued) ELSE Capacity
       IN /\ pending' = SubSeq(queued, 1, n)
          /\ queued' = SubSeq(queued, n + 1, Len(queued))
    /\ event' = "send"
    /\ UNCHANGED <<retired, abandoned, issued>>

Timeout ==
    /\ Len(pending) > 0
    /\ event' = "timeout"
    /\ UNCHANGED <<queued, pending, retired, abandoned, issued>>

Finish(lost) ==
    /\ Len(pending) > 0
    /\ retired' = retired \cup Elements(pending)
    /\ abandoned' = IF lost THEN abandoned \cup Elements(pending) ELSE abandoned
    /\ pending' = <<>>
    /\ event' = IF lost THEN "evicted" ELSE "reply"
    /\ UNCHANGED <<queued, issued>>

Next == Enqueue \/ Send \/ Timeout \/ Finish(TRUE) \/ Finish(FALSE) \/ UNCHANGED State

OrdersAccountedFor ==
    /\ Elements(queued) \cup Elements(pending) \cup retired = 1..issued
    /\ Len(queued) + Len(pending) + Cardinality(retired) = issued
    /\ abandoned \subseteq retired
    /\ Len(pending) <= Capacity
    /\ \A i, j \in 1..Len(pending) : i < j => pending[i] < pending[j]

SingleBatch == [][(Len(pending) > 0 /\ Len(pending') > 0) => pending' = pending]_State
BeforeEvictionWitness == Cardinality(abandoned) < 2

===============================================================================
