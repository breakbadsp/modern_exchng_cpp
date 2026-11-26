------------------------------- MODULE Gateway --------------------------------
EXTENDS Naturals, FiniteSets, Sequences

CONSTANTS Sessions, Slots, MaxGeneration, Mutate
VARIABLES socket, generation, session, recv, send, nextSeq, bytes, lastOrder

State == <<socket, generation, session, recv, send, nextSeq, bytes, lastOrder>>
Init ==
    /\ socket = [i \in Slots |-> FALSE]
    /\ generation = [i \in Slots |-> 0]
    /\ session = [i \in Slots |-> 0]
    /\ recv = [i \in Slots |-> 0]
    /\ send = [i \in Slots |-> 0]
    /\ nextSeq = [i \in Slots |-> 1]
    /\ bytes = [i \in Slots |-> <<>>]
    /\ lastOrder = [accepted |-> FALSE, session |-> 0]

Accept(i) ==
    /\ ~socket[i]
    /\ (recv[i] = 0 /\ send[i] = 0) \/ Mutate
    /\ generation[i] < MaxGeneration
    /\ socket' = [socket EXCEPT ![i] = TRUE]
    /\ generation' = [generation EXCEPT ![i] = @ + 1]
    /\ nextSeq' = [nextSeq EXCEPT ![i] = 1]
    /\ UNCHANGED <<session, recv, send, bytes, lastOrder>>

Logon(i, s) ==
    /\ socket[i] /\ session[i] = 0
    /\ \A j \in Slots : session[j] # s
    /\ session' = [session EXCEPT ![i] = s]
    /\ bytes' = [bytes EXCEPT ![i] = <<1>>]
    /\ nextSeq' = [nextSeq EXCEPT ![i] = 2]
    /\ UNCHANGED <<socket, generation, recv, send, lastOrder>>

QueueReport(i) ==
    /\ socket[i] /\ session[i] # 0 /\ nextSeq[i] <= 3
    /\ bytes' = [bytes EXCEPT ![i] = Append(@, nextSeq[i])]
    /\ nextSeq' = [nextSeq EXCEPT ![i] = @ + 1]
    /\ UNCHANGED <<socket, generation, session, recv, send, lastOrder>>

StartRecv(i) ==
    /\ socket[i] /\ recv[i] = 0
    /\ recv' = [recv EXCEPT ![i] = generation[i]]
    /\ UNCHANGED <<socket, generation, session, send, nextSeq, bytes, lastOrder>>

StartSend(i) ==
    /\ socket[i] /\ send[i] = 0 /\ Len(bytes[i]) > 0
    /\ send' = [send EXCEPT ![i] = generation[i]]
    /\ UNCHANGED <<socket, generation, session, recv, nextSeq, bytes, lastOrder>>

CloseSocket(i) ==
    /\ socket[i]
    /\ socket' = [socket EXCEPT ![i] = FALSE]
    /\ session' = [session EXCEPT ![i] = 0]
    /\ bytes' = [bytes EXCEPT ![i] = <<>>]
    /\ UNCHANGED <<generation, recv, send, nextSeq>>

Close(i) == CloseSocket(i) /\ UNCHANGED lastOrder

OrderFrame(i) ==
    /\ socket[i]
    /\ lastOrder' = [accepted |-> session[i] # 0, session |-> session[i]]
    /\ IF session[i] = 0 THEN CloseSocket(i)
       ELSE UNCHANGED <<socket, generation, session, recv, send, nextSeq, bytes>>

FailedLogon(i, s) ==
    /\ socket[i]
    /\ session[i] # 0 \/ (\E j \in Slots : session[j] = s)
    /\ Close(i)

Complete(i, g, isRecv) ==
    IF g # generation[i] THEN UNCHANGED State
    ELSE IF isRecv THEN
      /\ recv[i] # 0
      /\ recv' = [recv EXCEPT ![i] = 0]
      /\ UNCHANGED <<socket, generation, session, send, nextSeq, bytes, lastOrder>>
    ELSE
      /\ send[i] # 0
      /\ send' = [send EXCEPT ![i] = 0]
      /\ \E n \in 0..Len(bytes[i]) : bytes' = [bytes EXCEPT ![i] = SubSeq(@, n + 1, Len(@))]
      /\ UNCHANGED <<socket, generation, session, recv, nextSeq, lastOrder>>

Next ==
    \/ UNCHANGED State
    \/ \E i \in Slots :
         \/ Accept(i) \/ QueueReport(i) \/ StartRecv(i) \/ StartSend(i) \/ Close(i) \/ OrderFrame(i)
         \/ \E s \in Sessions : Logon(i, s) \/ FailedLogon(i, s)
         \/ \E g \in 1..MaxGeneration, r \in BOOLEAN : Complete(i, g, r)

SessionExclusive ==
    /\ \A i \in Slots : session[i] # 0 => socket[i]
    /\ \A i, j \in Slots : (i # j /\ session[i] # 0) => session[i] # session[j]

OrderRequiresLogon == lastOrder.accepted => lastOrder.session \in Sessions

BytesStayOrdered ==
    \A i \in Slots : \A n \in 2..Len(bytes[i]) : bytes[i][n - 1] + 1 = bytes[i][n]

CurrentBufferOwner ==
    \A i \in Slots : recv[i] \in {0, generation[i]} /\ send[i] \in {0, generation[i]}

ReuseAfterCompletion ==
    [][\A i \in Slots : generation'[i] # generation[i] => recv[i] = 0 /\ send[i] = 0]_State
===============================================================================
