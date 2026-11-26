-------------------------------- MODULE Frames --------------------------------
EXTENDS Naturals, Sequences

CONSTANTS MaxChunks, Mutate
VARIABLES input, delivered, open, chunks, received

State == <<input, delivered, open, chunks, received>>
Tokens == {"header", "body", "bad"}
Init == /\ input = <<>> /\ delivered = 0 /\ open = TRUE /\ chunks = 0 /\ received = <<>>
Read(chunk) ==
    /\ open /\ chunks < MaxChunks
    /\ chunks' = chunks + 1
    /\ input' = input \o chunk
    /\ received' = received \o chunk
    /\ UNCHANGED <<delivered, open>>

Process ==
    /\ open /\ Len(input) > 0
    /\ IF Head(input) # "header" THEN
           /\ open' = FALSE
           /\ UNCHANGED <<input, delivered>>
       ELSE
           /\ Len(input) > 1
           /\ IF input[2] = "body" THEN
                  /\ delivered' = delivered + 1
                  /\ input' = SubSeq(input, IF Mutate THEN 4 ELSE 3, Len(input))
                  /\ UNCHANGED open
              ELSE
                  /\ open' = FALSE /\ UNCHANGED <<input, delivered>>
    /\ UNCHANGED <<chunks, received>>

Next == Process \/ (\E a, b \in Tokens : Read(<<a>>) \/ Read(<<a, b>>)) \/ UNCHANGED State

FrameAccounting ==
    /\ Len(received) = 2 * delivered + Len(input)
    /\ SubSeq(received, 2 * delivered + 1, Len(received)) = input
    /\ \A i \in 1..delivered : received[2 * i - 1] = "header" /\ received[2 * i] = "body"

MalformedCloses ==
    open \/ (Len(input) > 0 /\ IF Head(input) # "header" THEN TRUE
                               ELSE Len(input) > 1 /\ input[2] # "body")
===============================================================================
