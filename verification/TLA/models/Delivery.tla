------------------------------- MODULE Delivery -------------------------------
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS MaxReports, MaxConnections, Mutate
VARIABLES connected, connection, issued, seq, queued, staged, dropped

State == <<connected, connection, issued, seq, queued, staged, dropped>>
Init ==
    /\ connected = FALSE /\ connection = 0 /\ issued = 0 /\ seq = 2
    /\ queued = <<>> /\ staged = <<>> /\ dropped = {}

Commit ==
    /\ issued < MaxReports
    /\ issued' = issued + 1
    /\ queued' = Append(queued, issued')
    /\ UNCHANGED <<connected, connection, seq, staged, dropped>>

Connect ==
    /\ ~connected /\ connection < MaxConnections
    /\ connected' = TRUE /\ connection' = connection + 1 /\ seq' = IF Mutate THEN seq ELSE 2
    /\ UNCHANGED <<issued, queued, staged, dropped>>

Disconnect ==
    /\ connected /\ connected' = FALSE
    /\ UNCHANGED <<connection, issued, seq, queued, staged, dropped>>

Route ==
    /\ Len(queued) > 0
    /\ queued' = Tail(queued)
    /\ IF connected THEN
          /\ staged' = Append(staged, [id |-> Head(queued), epoch |-> connection, seq |-> seq])
          /\ seq' = seq + 1
          /\ UNCHANGED dropped
       ELSE
          /\ dropped' = dropped \cup {Head(queued)}
          /\ UNCHANGED <<seq, staged>>
    /\ UNCHANGED <<connected, connection, issued>>

Overflow ==
    /\ connected /\ Len(queued) > 0
    /\ connected' = FALSE
    /\ queued' = Tail(queued)
    /\ dropped' = dropped \cup {Head(queued)}
    /\ UNCHANGED <<connection, issued, seq, staged>>

Next == Overflow \/ Commit \/ Connect \/ Disconnect \/ Route \/ UNCHANGED State

ReportAccounting ==
    LET sent == {staged[i].id : i \in 1..Len(staged)}
        pending == {queued[i] : i \in 1..Len(queued)}
    IN /\ sent \cup pending \cup dropped = 1..issued
       /\ Len(staged) + Len(queued) + Cardinality(dropped) = issued

SequencesPerConnection ==
    \A i \in 1..Len(staged) :
        staged[i].seq =
           2 + Cardinality({j \in 1..(i - 1) : staged[j].epoch = staged[i].epoch})
===============================================================================
