--------------------------------- MODULE Flow ---------------------------------
EXTENDS Naturals, Sequences

CONSTANTS Capacity, MaxCommand, EarlyStop
VARIABLES inbound, outbound, finished, issued, busy, phase, worker, gateway

State == <<inbound, outbound, finished, issued, busy, phase, worker, gateway>>
Init ==
    /\ inbound = <<>> /\ outbound = <<>> /\ finished = <<>>
    /\ issued = 0 /\ busy = <<>>
    /\ phase = "run" /\ worker = 0 /\ gateway = TRUE

Offer ==
    /\ phase = "run" /\ issued < MaxCommand
    /\ issued' = issued + 1
    /\ IF Len(inbound) < Capacity THEN
           /\ inbound' = Append(inbound, issued')
           /\ UNCHANGED busy
       ELSE
           /\ busy' = Append(busy, issued')
           /\ UNCHANGED inbound
    /\ UNCHANGED <<outbound, finished, phase, worker, gateway>>

StartCommand ==
    /\ phase = "run" /\ worker = 0 /\ Len(inbound) > 0
    /\ worker' = Head(inbound)
    /\ inbound' = Tail(inbound)
    /\ UNCHANGED <<outbound, finished, issued, busy, phase, gateway>>

Publish ==
    /\ worker # 0 /\ Len(outbound) < Capacity
    /\ outbound' = Append(outbound, worker)
    /\ worker' = 0
    /\ UNCHANGED <<inbound, finished, issued, busy, phase, gateway>>

Drain ==
    /\ gateway /\ Len(outbound) > 0
    /\ finished' = Append(finished, Head(outbound))
    /\ outbound' = Tail(outbound)
    /\ UNCHANGED <<inbound, issued, busy, phase, worker, gateway>>

RequestStop ==
    /\ phase = "run"
    /\ phase' = "stop"
    /\ UNCHANGED <<inbound, outbound, finished, issued, busy, worker, gateway>>

Join ==
    /\ phase = "stop" /\ worker = 0
    /\ phase' = "joined"
    /\ UNCHANGED <<inbound, outbound, finished, issued, busy, worker, gateway>>

StopGateway ==
    /\ gateway /\ (phase = "joined" \/ (EarlyStop /\ phase = "stop"))
    /\ gateway' = FALSE
    /\ UNCHANGED <<inbound, outbound, finished, issued, busy, phase, worker>>

Next == Offer \/ StartCommand \/ Publish \/ Drain \/ RequestStop \/ Join \/ StopGateway \/ UNCHANGED State

Spec == Init /\ [][Next]_State /\ WF_State(Publish) /\ WF_State(Drain)
        /\ WF_State(Join) /\ WF_State(StopGateway)

InOrder(xs) == \A i, j \in 1..Len(xs) : i < j => xs[i] < xs[j]
Accepted == finished \o outbound \o (IF worker = 0 THEN <<>> ELSE <<worker>>) \o inbound
QueueSafe ==
    /\ Len(inbound) <= Capacity /\ Len(outbound) <= Capacity
    /\ InOrder(Accepted)
    /\ \A i \in 1..Len(busy), j \in 1..Len(Accepted) : busy[i] # Accepted[j]
    /\ Len(busy) + Len(Accepted) = issued

DrainPath == gateway \/ worker = 0
ShutdownProgress == (phase = "stop") ~> (phase = "joined" /\ ~gateway)
===============================================================================
