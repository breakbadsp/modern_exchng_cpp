------------------------- MODULE MarketDataHandoff -------------------------
EXTENDS Naturals, FiniteSets

CONSTANTS WritesMax, Broken
Slots == 0..2
VARIABLES writer, reader, spare, ready, first, second, published, expected, pending
vars == <<writer, reader, spare, ready, first, second, published, expected, pending>>

Init ==
    /\ writer = 0 /\ reader = 1 /\ spare = 2 /\ ready = FALSE
    /\ first = [slot \in Slots |-> 0] /\ second = first
    /\ published = 0 /\ expected = 0 /\ pending = FALSE

WriteFirst ==
    /\ published < WritesMax /\ first[writer] # published + 1
    /\ first' = [first EXCEPT ![writer] = published + 1]
    /\ UNCHANGED <<writer, reader, spare, ready, second, published, expected, pending>>

WriteSecond ==
    /\ first[writer] = published + 1 /\ second[writer] # first[writer]
    /\ second' = [second EXCEPT ![writer] = first[writer]]
    /\ UNCHANGED <<writer, reader, spare, ready, first, published, expected, pending>>

Publish ==
    /\ second[writer] = published + 1
    /\ writer' = spare /\ spare' = writer /\ ready' = TRUE
    /\ published' = published + 1
    /\ UNCHANGED <<reader, first, second, expected, pending>>

Take ==
    /\ ready /\ ~pending
    /\ reader' = spare /\ spare' = (IF Broken THEN spare ELSE reader) /\ ready' = FALSE
    /\ expected' = published /\ pending' = TRUE
    /\ UNCHANGED <<writer, first, second, published>>

Read ==
    /\ pending /\ pending' = FALSE
    /\ UNCHANGED <<writer, reader, spare, ready, first, second, published, expected>>

Next == WriteFirst \/ WriteSecond \/ Publish \/ Take \/ Read
Spec == Init /\ [][Next]_vars

OwnSlots == Cardinality({writer, reader, spare}) = 3
WholeRead == pending => first[reader] = expected /\ second[reader] = expected
LatestAvailable ==
    IF ready THEN first[spare] = published /\ second[spare] = published
             ELSE expected = published
=============================================================================
