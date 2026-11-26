-------------------------------- MODULE Frames --------------------------------
EXTENDS Naturals, Sequences

CONSTANTS MaxChunks, Mutation
VARIABLES input, delivered, open, chunks, received, action

State == <<input, delivered, open, chunks, received, action>>
Tokens == {"header", "body", "bad"}
Init == /\ input = <<>> /\ delivered = 0 /\ open = TRUE /\ chunks = 0 /\ received = <<>>
        /\ action = "none"

Malformed(xs) ==
    IF Len(xs) = 0 THEN FALSE
    ELSE IF Head(xs) # "header" THEN TRUE
    ELSE IF Len(xs) > 1 THEN xs[2] # "body" ELSE FALSE

Read(chunk) ==
    /\ open /\ chunks < MaxChunks
    /\ chunks' = chunks + 1
    /\ input' = input \o chunk
    /\ received' = received \o chunk
    /\ action' = "read"
    /\ UNCHANGED <<delivered, open>>

Process ==
    /\ open /\ Len(input) > 0
    /\ IF Malformed(input) THEN
           /\ open' = (Mutation = "keep-open")
           /\ action' = "malformed"
           /\ UNCHANGED <<input, delivered>>
       ELSE
           /\ Len(input) > 1
           /\ delivered' = delivered + 1
           /\ input' = SubSeq(input, IF Mutation = "skip-token" THEN 4 ELSE 3, Len(input))
           /\ action' = "frame"
           /\ UNCHANGED open
    /\ UNCHANGED <<chunks, received>>

Next == Process \/ (\E a, b \in Tokens : Read(<<a>>) \/ Read(<<a, b>>)) \/ UNCHANGED State
Spec == Init /\ [][Next]_State /\ WF_State(Process)

FrameAccounting ==
    /\ Len(received) = 2 * delivered + Len(input)
    /\ SubSeq(received, 2 * delivered + 1, Len(received)) = input
    /\ \A i \in 1..delivered : received[2 * i - 1] = "header" /\ received[2 * i] = "body"

MalformedCloses == action = "malformed" => ~open
RejectionProgress == (open /\ Malformed(input)) ~> ~open
===============================================================================
