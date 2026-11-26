------------------------------ MODULE FaultGame -------------------------------
EXTENDS Naturals, FiniteSets

CONSTANTS Replicas, Mutate
VARIABLES replica, phase, outcome, sampledOps, sampledDigests

State == <<replica, phase, outcome, sampledOps, sampledDigests>>
Init ==
    /\ replica = [i \in Replicas |-> "running"]
    /\ phase = "idle" /\ outcome = "none"
    /\ sampledOps = {} /\ sampledDigests = {}

Fault(i, status) ==
    /\ phase = "idle"
    /\ status # "frozen" \/ replica[i] = "running"
    /\ replica' = [replica EXCEPT ![i] = status]
    /\ UNCHANGED <<phase, outcome, sampledOps, sampledDigests>>

Verify ==
    /\ phase = "idle"
    /\ phase' = "probe"
    /\ replica' = [i \in Replicas |-> "running"]
    /\ UNCHANGED <<outcome, sampledOps, sampledDigests>>

Collect ==
    /\ phase = "probe"
    /\ phase' = "result"
    /\ replica' = [i \in Replicas |-> "stopped"]
    /\ \E ops \in [Replicas -> 0..2], digests \in [Replicas -> 0..1] :
        /\ sampledOps' = {ops[i] : i \in Replicas}
        /\ sampledDigests' = {digests[i] : i \in Replicas}
        /\ outcome' = IF ~Mutate /\ (Cardinality(sampledOps') # 1 \/ 0 \in sampledOps')
                      THEN "inconclusive"
                      ELSE IF Cardinality(sampledDigests') = 1 THEN "passed" ELSE "failed"

Restart ==
    /\ phase = "result"
    /\ phase' = "idle"
    /\ replica' = [i \in Replicas |-> "running"]
    /\ UNCHANGED <<outcome, sampledOps, sampledDigests>>

Next == Verify \/ Collect \/ Restart
        \/ (\E i \in Replicas, s \in {"running", "frozen", "stopped"} : Fault(i, s))
        \/ UNCHANGED State

AgreementOnlyAtSameCommit ==
    outcome = "passed" =>
      Cardinality(sampledOps) = 1 /\ 0 \notin sampledOps /\ Cardinality(sampledDigests) = 1
VerificationOwnsReplicas ==
    /\ phase = "probe" => \A i \in Replicas : replica[i] = "running"
    /\ phase = "result" => \A i \in Replicas : replica[i] = "stopped"
===============================================================================
