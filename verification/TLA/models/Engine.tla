-------------------------------- MODULE Engine --------------------------------
EXTENDS Naturals, FiniteSets

CONSTANTS Sessions, Orders, MaxRequest, Mutate
VARIABLES marks, leaves, filled, generation, output

Keys == Sessions \X Orders
NoKey == <<0, 0>>
State == <<marks, leaves, filled, generation, output>>
Live == {k \in Keys : leaves[k] > 0}
Report(kind, key, version, changed) ==
    [kind |-> kind, key |-> key, version |-> version, changed |-> changed]

Init ==
    /\ marks = [s \in Sessions |-> 0]
    /\ leaves = [k \in Keys |-> 0]
    /\ filled = [k \in Keys |-> 0]
    /\ generation = [k \in Keys |-> 0]
    /\ output = Report("none", NoKey, 0, FALSE)

Request(k, n, kind, total, samePrice) ==
    LET known == k \in Live
        duplicate == n <= marks[k[1]]
        changes == ~duplicate \/ (Mutate /\ kind = "cancel")
        allowed == CASE kind = "new" -> ~known /\ total > 0
                     [] kind = "cancel" -> known
                     [] OTHER -> known /\ total > filled[k]
        good == changes /\ allowed
        reduce == kind = "replace" /\ samePrice /\ total < filled[k] + leaves[k]
        nextLeaves == CASE ~good -> leaves
                        [] kind = "new" -> [leaves EXCEPT ![k] = total]
                        [] kind = "cancel" -> [leaves EXCEPT ![k] = 0]
                        [] OTHER -> [leaves EXCEPT ![k] = total - filled[k]]
        nextFilled == IF good /\ kind = "new" THEN [filled EXCEPT ![k] = 0] ELSE filled
        nextGeneration == IF good /\ (kind = "new" \/ (kind = "replace" /\ ~reduce))
                          THEN [generation EXCEPT ![k] = @ + 1] ELSE generation
        status == IF duplicate THEN "duplicate" ELSE IF good THEN kind ELSE "reject"
    IN /\ marks' = IF duplicate THEN marks ELSE [marks EXCEPT ![k[1]] = n]
       /\ leaves' = nextLeaves
       /\ filled' = nextFilled
       /\ generation' = nextGeneration
       /\ output' = Report(status, k, nextGeneration[k],
                           <<nextLeaves, nextFilled, nextGeneration>> # <<leaves, filled, generation>>)

Fill(k) ==
    /\ k \in Live
    /\ filled' = [filled EXCEPT ![k] = @ + 1]
    /\ leaves' = [leaves EXCEPT ![k] = @ - 1]
    /\ output' = Report("fill", k, generation[k], TRUE)
    /\ UNCHANGED <<marks, generation>>

Next ==
    \/ \E k \in Keys, n \in 1..MaxRequest, qty \in 0..2 :
         \/ Request(k, n, "new", qty, FALSE)
         \/ Request(k, n, "cancel", 0, FALSE)
         \/ \E samePrice \in BOOLEAN : Request(k, n, "replace", qty, samePrice)
    \/ \E k \in Keys : Fill(k)
    \/ UNCHANGED State

OwnershipAndQuantity ==
    /\ leaves \in [Keys -> 0..2]
    /\ filled \in [Keys -> 0..2]
    /\ \A k \in Keys : leaves[k] + filled[k] <= 2
    /\ \A k \in Live : generation[k] > 0
    /\ output.key \in Keys \cup {NoKey}
    /\ output.kind = "fill" => output.version = generation[output.key]

DuplicateCannotChangeOrders == output.kind = "duplicate" => ~output.changed

IsolatedOrderUpdates ==
    [][\A k \in Keys :
        k # output'.key => UNCHANGED <<leaves[k], filled[k], generation[k]>>]_State

MarksAreMonotonic ==
    [][\A s \in Sessions : marks'[s] >= marks[s]]_State
===============================================================================
