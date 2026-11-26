------------------------------- MODULE Delivery -------------------------------
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS MaxReports, MaxConnections, Sessions, Mutate
VARIABLES session, connection, issued, seq, queued, staged, dropped, previousSession

State == <<session, connection, issued, seq, queued, staged, dropped, previousSession>>
Init ==
    /\ session = 0 /\ connection = 0 /\ issued = 0 /\ seq = 2
    /\ queued = <<>> /\ staged = <<>> /\ dropped = {} /\ previousSession = 0

Commit(owner) ==
    /\ issued < MaxReports
    /\ issued' = issued + 1
    /\ queued' = Append(queued, [id |-> issued', owner |-> owner])
    /\ UNCHANGED <<session, connection, seq, staged, dropped, previousSession>>

Connect(owner) ==
    /\ session = 0 /\ connection < MaxConnections
    /\ session' = owner /\ connection' = connection + 1 /\ seq' = 2
    /\ UNCHANGED <<issued, queued, staged, dropped, previousSession>>

Disconnect ==
    /\ session # 0 /\ session' = 0 /\ previousSession' = session
    /\ UNCHANGED <<connection, issued, seq, queued, staged, dropped>>

Route ==
    /\ Len(queued) > 0
    /\ queued' = Tail(queued)
    /\ LET report == Head(queued)
       IN IF session = report.owner \/ (Mutate /\ session # 0 /\ previousSession = report.owner) THEN
          /\ staged' = Append(staged, [id |-> report.id, owner |-> report.owner,
                                       recipient |-> session, epoch |-> connection, seq |-> seq])
          /\ seq' = seq + 1
          /\ UNCHANGED dropped
       ELSE
          /\ dropped' = dropped \cup {report.id}
          /\ UNCHANGED <<seq, staged>>
    /\ UNCHANGED <<session, connection, issued, previousSession>>

Overflow ==
    /\ Len(queued) > 0 /\ session = Head(queued).owner
    /\ session' = 0 /\ previousSession' = session
    /\ queued' = Tail(queued)
    /\ dropped' = dropped \cup {Head(queued).id}
    /\ UNCHANGED <<connection, issued, seq, staged>>

Next == Overflow \/ Disconnect \/ Route \/ UNCHANGED State
        \/ (\E user \in Sessions : Commit(user) \/ Connect(user))

ReportAccounting ==
    LET sent == {staged[i].id : i \in 1..Len(staged)}
        pending == {queued[i].id : i \in 1..Len(queued)}
    IN /\ sent \cup pending \cup dropped = 1..issued
       /\ Len(staged) + Len(queued) + Cardinality(dropped) = issued

ReportKeepsOwner ==
    \A i \in 1..Len(staged) : staged[i].owner = staged[i].recipient

SequencesPerConnection ==
    \A i \in 1..Len(staged) :
        staged[i].seq =
           2 + Cardinality({j \in 1..(i - 1) : staged[j].epoch = staged[i].epoch})
===============================================================================
