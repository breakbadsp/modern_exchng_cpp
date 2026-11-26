------------------------------ MODULE Matching ------------------------------
EXTENDS Integers, FiniteSets, Sequences, TLC

CONSTANTS Capacity, Levels, MaxSteps, Prices, Quantities, MutateReduce, TargetCapacity
VARIABLES book, step, last

Sides == {1, 2}
Ids(b) == {b[i].id : i \in 1..Len(b)}
Remove(b, i) == SubSeq(b, 1, i - 1) \o SubSeq(b, i + 1, Len(b))
SidePrices(b, s) == {b[i].price : i \in {j \in 1..Len(b) : b[j].side = s}}
Room(b, s, p) == p \in SidePrices(b, s) \/ Cardinality(SidePrices(b, s)) < Levels
Order(id, s, p, q) == [id |-> id, side |-> s, price |-> p, qty |-> q]
Command(k, id, s, p, q) == [kind |-> k, id |-> id, side |-> s, price |-> p, qty |-> q]
Eligible(o, c) ==
    o.side # c.side /\ (c.kind = "M" \/ IF c.side = 1 THEN o.price <= c.price ELSE o.price >= c.price)

Better(b, i, j, s) ==
    IF b[i].price = b[j].price THEN i < j
    ELSE IF s = 1 THEN b[i].price < b[j].price ELSE b[i].price > b[j].price

RECURSIVE Sweep(_, _, _)
Sweep(b, c, q) ==
    LET eligible == {i \in 1..Len(b) : Eligible(b[i], c)}
    IN IF q = 0 \/ eligible = {} THEN [book |-> b, leaves |-> q, fills |-> <<>>]
       ELSE LET i == CHOOSE k \in eligible : \A j \in eligible : j = k \/ Better(b, k, j, c.side)
                maker == b[i]
                traded == IF maker.qty < q THEN maker.qty ELSE q
                remainder == IF maker.qty = traded THEN Remove(b, i)
                             ELSE [b EXCEPT ![i].qty = @ - traded]
                tail == Sweep(remainder, c, q - traded)
            IN [tail EXCEPT !.fills = <<[id |-> maker.id, price |-> maker.price, qty |-> traded]>> \o @]

Result(b, accepted, filled, leaves, fills) ==
    [book |-> b, accepted |-> accepted, filled |-> filled, leaves |-> leaves, fills |-> fills]

Submit(b, c) ==
    LET out == Sweep(b, c, c.qty)
        filled == c.qty - out.leaves
        rests == c.kind # "M" /\ out.leaves > 0 /\ Len(out.book) < Capacity
                 /\ Room(out.book, c.side, c.price)
        next == IF rests THEN Append(out.book, Order(c.id, c.side, c.price, out.leaves)) ELSE out.book
    IN Result(next, rests \/ filled > 0, filled, IF c.kind = "M" THEN 0 ELSE out.leaves, out.fills)

Execute(b, c) ==
    CASE c.kind \in {"L", "M"} -> Submit(b, c)
      [] c.id \notin Ids(b) -> Result(b, FALSE, 0, IF c.kind = "R" THEN c.qty ELSE 0, <<>>)
      [] OTHER ->
        LET i == CHOOSE j \in 1..Len(b) : b[j].id = c.id
            old == b[i]
            without == Remove(b, i)
        IN CASE c.kind = "C" -> Result(without, TRUE, 0, 0, <<>>)
             [] c.kind = "D" ->
                IF c.qty >= old.qty THEN Result(b, FALSE, 0, 0, <<>>)
                ELSE LET reduced == [old EXCEPT !.qty = c.qty]
                     IN Result(IF MutateReduce THEN Append(without, reduced)
                               ELSE [b EXCEPT ![i] = reduced], TRUE, 0, 0, <<>>)
             [] OTHER ->
                IF Room(without, old.side, c.price) THEN Submit(without, c)
                ELSE Result(b, FALSE, 0, c.qty, <<>>)

AllCommands ==
    {Command("L", step + 1, s, p, q) : s \in Sides, p \in Prices, q \in Quantities}
    \cup {Command("M", step + 1, s, 0, q) : s \in Sides, q \in Quantities}
    \cup {Command("C", id, 0, 0, 0) : id \in Ids(book) \cup {0}}
    \cup {Command("D", id, 0, 0, q) : id \in Ids(book), q \in Quantities}
    \cup {Command("R", book[i].id, book[i].side, p, q) :
          i \in 1..Len(book), p \in Prices, q \in Quantities}

CapacityPrefix ==
    <<Command("L", 1, 1, 1, 2), Command("L", 2, 1, 1, 2),
      Command("L", 3, 1, 0, 1), Command("L", 4, 2, 2, 2),
      Command("R", 1, 1, 2, 2)>>

Commands == IF TargetCapacity THEN {CapacityPrefix[step + 1]} ELSE AllCommands

Init == /\ book = <<>>
        /\ step = 0
        /\ last = [command |-> Command("C", 0, 0, 0, 0), before |-> <<>>,
                    accepted |-> FALSE, filled |-> 0, leaves |-> 0, fills |-> <<>>]

Next ==
    /\ step < MaxSteps
    /\ \E c \in Commands :
        LET r == Execute(book, c)
        IN /\ book' = r.book
           /\ step' = step + 1
           /\ last' = [command |-> c, before |-> book, accepted |-> r.accepted,
                        filled |-> r.filled, leaves |-> r.leaves, fills |-> r.fills]

BookSafe ==
    /\ Len(book) <= Capacity
    /\ Cardinality(Ids(book)) = Len(book)
    /\ \A i \in 1..Len(book) : book[i].qty > 0
    /\ \A s \in Sides : Cardinality(SidePrices(book, s)) <= Levels
    /\ \A i, j \in 1..Len(book) :
         (book[i].side = 1 /\ book[j].side = 2) => book[i].price < book[j].price

RECURSIVE SumFills(_)
SumFills(f) == IF Len(f) = 0 THEN 0 ELSE Head(f).qty + SumFills(Tail(f))

PriorityAndQuantity ==
    LET c == last.command
        b == last.before
        fills == last.fills
    IN /\ last.filled = SumFills(fills)
       /\ last.filled <= c.qty
       /\ IF c.kind = "M" THEN c.id \notin Ids(book) /\ last.leaves = 0
          ELSE IF c.kind \in {"L", "R"} THEN last.filled + last.leaves = c.qty ELSE TRUE
       /\ ~last.accepted => book = b /\ Len(fills) = 0
       /\ \A n \in 1..Len(fills) :
            LET f == fills[n]
                used == {fills[k].id : k \in 1..(n - 1)}
                i == CHOOSE j \in 1..Len(b) : b[j].id = f.id
            IN /\ f.id \in Ids(b) \ used
               /\ b[i].side # c.side /\ f.price = b[i].price /\ f.qty \in 1..b[i].qty
               /\ IF c.kind = "M" THEN TRUE
                  ELSE IF c.side = 1 THEN f.price <= c.price ELSE f.price >= c.price
               /\ n < Len(fills) => f.qty = b[i].qty
               /\ IF f.qty = b[i].qty THEN f.id \notin Ids(book)
                  ELSE \E k \in 1..Len(book) : book[k].id = f.id /\ book[k].qty = b[i].qty - f.qty
               /\ \A j \in 1..Len(b) :
                     (b[j].id \notin used /\ b[j].side # c.side /\ j # i)
                         => IF b[j].price = f.price THEN i < j
                            ELSE IF c.side = 1 THEN f.price < b[j].price ELSE f.price > b[j].price

ReduceKeepsPriority ==
    (last.command.kind = "D" /\ last.accepted) =>
        \A i \in 1..Len(book) : book[i].id = last.before[i].id

BeforeReduceWitness ==
    ~(last.command.kind = "D" /\ last.accepted /\ Len(book) = 2
      /\ book[1].id = last.command.id /\ book[1].side = book[2].side /\ book[1].price = book[2].price)

ReplaceCapacityIsAtomic ==
    IF TargetCapacity /\ step = Len(CapacityPrefix)
    THEN ~last.accepted /\ Len(last.fills) = 0 /\ book = last.before
    ELSE TRUE

BeforeCapacityWitness == ~TargetCapacity \/ step < Len(CapacityPrefix)

BeforeSweepWitness ==
    IF Len(last.fills) = 2 THEN last.fills[1].price = last.fills[2].price ELSE TRUE

=============================================================================
