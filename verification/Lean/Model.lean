import Lean

set_option warningAsError true

namespace Mex

structure Order where
  id : Nat
  buy : Bool
  price : Int
  qty : Nat

structure Command where
  kind : Char
  id : Nat
  buy : Bool := false
  price : Int := 0
  qty : Nat := 0

structure Limits where
  orders : Nat
  levels : Nat

structure Result where
  book : List Order
  accepted : Bool := false
  filled : Nat := 0
  leaves : Nat := 0
  fills : List Order := []

def better (buy : Bool) (a b : Order) : Bool :=
  if buy then a.price >= b.price else a.price <= b.price

def eligible (cmd : Command) (maker : Order) : Bool :=
  maker.buy != cmd.buy &&
    (cmd.kind == 'M' || if cmd.buy then maker.price <= cmd.price else maker.price >= cmd.price)

def fill (qty : Nat) (makers : List Order) : List Order :=
  match makers with
  | [] => []
  | maker :: tail =>
    if qty == 0 then [] else
      { maker with qty := min qty maker.qty } :: fill (qty - maker.qty) tail

theorem fill_bounded (qty : Nat) (makers : List Order) :
    ((fill qty makers).map (·.qty)).sum ≤ qty := by
  induction makers generalizing qty with
  | nil => simp [fill]
  | cons maker tail ih =>
    by_cases h : qty = 0
    · simp [fill, h]
    · simp only [fill, beq_iff_eq, h, if_false, List.map_cons, List.sum_cons]
      have remaining := ih (qty - maker.qty)
      omega

def room (limits : Limits) (book : List Order) (cmd : Command) : Bool :=
  let prices := (book.filter (·.buy == cmd.buy)).map (·.price)
  prices.contains cmd.price || prices.eraseDups.length < limits.levels

def submit (limits : Limits) (book : List Order) (cmd : Command) : Result :=
  -- A stable sort preserves time priority within each price.
  let makers := (book.filter (eligible cmd)).mergeSort (better (!cmd.buy))
  let fills := fill cmd.qty makers
  let filled := (fills.map (·.qty)).sum
  let remaining := cmd.qty - filled
  let next := book.filterMap fun (order : Order) =>
    let traded : Nat := ((fills.find? (·.id == order.id)).map (·.qty)).getD 0
    if traded < order.qty then some { order with qty := order.qty - traded } else none
  let rests := cmd.kind != 'M' && remaining > 0 &&
    next.length < limits.orders && room limits next cmd
  { book := if rests then next ++ [⟨cmd.id, cmd.buy, cmd.price, remaining⟩] else next
    accepted := rests || filled > 0
    filled
    leaves := if cmd.kind == 'M' then 0 else remaining
    fills }

def step (limits : Limits) (book : List Order) (cmd : Command) : Result :=
  if cmd.kind == 'L' || cmd.kind == 'M' then submit limits book cmd else
    match book.find? (·.id == cmd.id) with
    | none => { book, leaves := if cmd.kind == 'R' then cmd.qty else 0 }
    | some old =>
      let without := book.filter (·.id != old.id)
      match cmd.kind with
      | 'C' => { book := without, accepted := true }
      | 'D' =>
        if cmd.qty < old.qty then
          { book := book.map fun order =>
              if order.id == old.id then { order with qty := cmd.qty } else order
            accepted := true }
        else { book }
      | _ =>
        let replacement := { cmd with buy := old.buy }
        if room limits without replacement then submit limits without replacement
        else { book, leaves := cmd.qty }

def sideNumber (buy : Bool) : Nat := if buy then 1 else 2

def commandLine (cmd : Command) : String :=
  s!"{cmd.kind} {cmd.id} {sideNumber cmd.buy} {cmd.price} {cmd.qty}"

def resultLine (result : Result) : String :=
  let orders := result.book.mergeSort fun a b =>
    if a.buy == b.buy then a.price <= b.price else a.buy
  let resting := orders.map fun o => s!" {o.id}:{sideNumber o.buy}:{o.price}:{o.qty}"
  let fills := result.fills.map fun o => s!" {o.id}:{o.price}:{o.qty}"
  let accepted : Nat := if result.accepted then 1 else 0
  s!"{accepted} {result.filled} {result.leaves} |{String.join fills} |{String.join resting}"

def matchingTrace (label : String) (limits : Limits) (commands : List Command) : Lean.Json := Id.run do
  let mut book := []
  let mut expected := #[]
  for cmd in commands do
    let result := step limits book cmd
    expected := expected.push (resultLine result)
    book := result.book
  return Lean.toJson ("matching", label, #[s!"{limits.orders} {limits.levels}"] ++
    (commands.map commandLine).toArray, expected)

def add (id : Nat) (buy : Bool) (price : Int) (qty : Nat) : Command :=
  ⟨'L', id, buy, price, qty⟩

def fifo : List Command :=
  [add 1 false 100 4, add 2 false 100 4, add 3 false 101 2,
   ⟨'D', 1, false, 0, 2⟩, ⟨'M', 5, true, 0, 5⟩,
   ⟨'R', 2, false, 102, 4⟩, add 7 true 102 8, ⟨'C', 7, false, 0, 0⟩]

def capacity : List Command :=
  [add 1 true 1 2, add 2 true 1 2, add 3 true 0 1, add 4 false 2 2,
   ⟨'R', 1, true, 2, 2⟩, add 6 true 1 1, add 7 true 2 3, add 8 true 2 1,
   add 9 true 2 0, ⟨'M', 10, false, 0, 0⟩]

def draw (bound : Nat) : StateM UInt64 Nat := do
  let next := (← get) * 6364136223846793005 + 1442695040888963407
  set next
  return (next >>> 32).toNat % bound

def generatedCommands (seed : Nat) : List Command := Id.run do
  let generate : StateM UInt64 (List Command) := do
    let mut result := []
    for id in [1:257] do
      let kind := (['L', 'L', 'L', 'M', 'C', 'D', 'R'])[← draw 7]!
      let distance ← draw 12
      let target := if kind == 'L' then id else id - distance - 1
      let buy := (← draw 2) == 0
      let price := Int.ofNat (← draw 9) - 4
      let qty := (← draw 8) + 1
      result := ⟨kind, target, buy, price, if kind == 'C' then 0 else qty⟩ :: result
    return result.reverse
  return (generate.run seed.toUInt64).1

-- The C++ ladder is worst-first. A cursor from its end visits exactly
-- the reversal of the selected suffix.
def cursorCapture {α : Type} (worstFirst : List α) (depth : Nat) : List α :=
  (worstFirst.drop (worstFirst.length - depth)).reverse

theorem cursorCapture_eq {α : Type} (levels : List α) (depth : Nat) :
    cursorCapture levels depth = levels.reverse.take depth := by
  unfold cursorCapture
  rw [List.reverse_drop]
  -- Clamp an oversize request to the available levels.
  by_cases h : depth ≤ levels.length
  · rw [Nat.sub_sub_self h]
  · simp only [Nat.sub_eq_zero_of_le (Nat.le_of_not_ge h), Nat.sub_zero]
    rw [List.take_of_length_le (by simp),
        List.take_of_length_le (by simpa using Nat.le_of_not_ge h)]

theorem capture_bound {α : Type} (levels : List α) (depth : Nat) :
    (cursorCapture levels depth).length = min depth levels.length := by
  simp [cursorCapture_eq]

def depthLine (orders : List Order) (buy : Bool) : String :=
  let onSide := orders.filter (·.buy == buy)
  let prices := ((onSide.mergeSort (better buy)).map (·.price)).eraseDups
  let entries := (prices.take 20).map fun price =>
    let qty := ((onSide.filter (·.price == price)).map (·.qty)).sum
    s!" {price}:{qty}"
  String.join entries

def depthTrace (label : String) (orders : List Order) (cancels : List Nat := []) : Lean.Json :=
  let limits := orders.zipIdx
  let adds := limits.map fun (order, index) => commandLine (add index order.buy order.price order.qty)
  let removes := cancels.map fun id => commandLine ⟨'C', id, false, 0, 0⟩
  let resting := (limits.filter (fun (_, index) => !cancels.contains index)).map Prod.fst
  let commands := adds ++ removes
  Lean.toJson ("depth", label, (toString commands.length :: commands).toArray,
    #[s!"B{depthLine resting true} | A{depthLine resting false}"])

def depthOrders (bids asks : Nat) : List Order :=
  let side (buy : Bool) (n : Nat) : List Order :=
    (List.range n).flatMap fun index =>
      let offset := Int.ofNat index
      let price := if buy then -offset else offset + 1
      [⟨0, buy, price, 4294967295⟩, ⟨0, buy, price, index + 1⟩]
  side true bids ++ (side false asks).reverse

def run : IO Unit := do
  IO.println (matchingTrace "FIFO and multi-price sweep" ⟨8, 4⟩ fifo).compress
  IO.println (matchingTrace "order and level capacity" ⟨4, 2⟩ capacity).compress
  for seed in [0:64] do
    let limits : Limits := ⟨seed % 12 + 1, seed % 5⟩
    IO.println (matchingTrace s!"seed {seed}" limits (generatedCommands seed)).compress
  for bids in [0, 1, 19, 20, 21, 32] do
    for asks in [0, 1, 19, 20, 21, 32] do
      IO.println (depthTrace s!"depth {bids}/{asks}" (depthOrders bids asks)).compress
  IO.println (depthTrace "cancel promotes a hidden bid" (depthOrders 21 21) [0, 1]).compress
  IO.println (depthTrace "cancel promotes a hidden ask" (depthOrders 21 21) [82, 83]).compress

end Mex

def main : IO Unit := Mex.run
