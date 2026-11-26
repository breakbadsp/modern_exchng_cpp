------------------------------- MODULE Loadgen --------------------------------
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS Capacity, MaxRequest, Mutate
VARIABLES tracked, request, command

State == <<tracked, request, command>>
Init == /\ tracked = <<>> /\ request = 0
        /\ command = [kind |-> "none", id |-> 0, before |-> <<>>]

New ==
    /\ Len(tracked) < Capacity
    /\ tracked' = Append(tracked, request + 1)
    /\ command' = [kind |-> "new", id |-> request + 1, before |-> tracked]

Cancel ==
    /\ Len(tracked) > 0
    /\ tracked' = Tail(tracked)
    /\ command' = [kind |-> "cancel", id |-> Head(tracked), before |-> tracked]

Replace ==
    /\ \E i \in 1..Len(tracked) :
        command' = [kind |-> "replace", id |-> IF Mutate THEN 0 ELSE tracked[i], before |-> tracked]
    /\ UNCHANGED tracked

Next ==
    \/ /\ request < MaxRequest
       /\ request' = request + 1
       /\ New \/ Cancel \/ Replace
    \/ UNCHANGED State

TrackedOrders ==
    /\ Len(tracked) <= Capacity
    /\ \A i, j \in 1..Len(tracked) : i < j => tracked[i] < tracked[j]
    /\ command.kind \in {"cancel", "replace"} =>
         \E i \in 1..Len(command.before) : command.before[i] = command.id

RequestNumbers == [][request' = request \/ request' = request + 1]_State
===============================================================================
