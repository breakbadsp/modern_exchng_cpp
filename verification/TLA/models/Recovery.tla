------------------------------- MODULE Recovery -------------------------------
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS LastCommit, Mutate
VARIABLES log, applied, total, snapshot, restartCutoff, startTime, emitted, restarted

State == <<log, applied, total, snapshot, restartCutoff, startTime, emitted, restarted>>
RECURSIVE ApplyPrefix(_)
ApplyPrefix(xs) == IF Len(xs) = 0 THEN 0
                   ELSE 3 * ApplyPrefix(SubSeq(xs, 1, Len(xs) - 1)) + xs[Len(xs)].value
Prefix(n) == SubSeq(log, 1, n)

Init ==
    /\ log = <<>>
    /\ applied = 0
    /\ total = 0
    /\ snapshot = [op |-> 0, total |-> 0]
    /\ restartCutoff = 0
    /\ startTime = 0
    /\ emitted = {}
    /\ restarted = FALSE

Commit ==
    /\ Len(log) < LastCommit
    /\ \E v \in {1, 2}, t \in 1..LastCommit :
         /\ IF Len(log) = 0 THEN TRUE ELSE t >= log[Len(log)].time
         /\ log' = Append(log, [value |-> v, time |-> t])
    /\ UNCHANGED <<applied, total, snapshot, restartCutoff, startTime, emitted, restarted>>

Apply ==
    /\ applied < Len(log)
    /\ applied' = applied + 1
    /\ total' = 3 * total + log[applied'].value
    /\ emitted' = IF Mutate \/ (applied' > restartCutoff /\ log[applied'].time >= startTime)
                   THEN emitted \cup {applied'} ELSE emitted
    /\ UNCHANGED <<log, snapshot, restartCutoff, startTime, restarted>>

Snapshot ==
    /\ snapshot' = [op |-> applied, total |-> total]
    /\ UNCHANGED <<log, applied, total, restartCutoff, startTime, emitted, restarted>>

Restart ==
    /\ ~restarted
    /\ applied' = snapshot.op
    /\ total' = snapshot.total
    /\ restartCutoff' = applied
    /\ startTime' \in 1..LastCommit
    /\ emitted' = {}
    /\ restarted' = TRUE
    /\ UNCHANGED <<log, snapshot>>

Next == Commit \/ Apply \/ Snapshot \/ Restart \/ UNCHANGED State

PrefixAgreement == total = ApplyPrefix(Prefix(applied)) /\ snapshot.total = ApplyPrefix(Prefix(snapshot.op))
OldReportsSuppressed ==
    \A n \in emitted : n > restartCutoff /\ log[n].time >= startTime
===============================================================================
