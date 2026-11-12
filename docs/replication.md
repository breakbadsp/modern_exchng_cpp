# Replication

The exchange can run as a cluster of copies (replicas). The cluster continues
to serve clients when a minority of the replicas fails: one of three, or two
of five. An order that a client got a report for is not lost when replicas
crash, or when all replicas lose power.

[libvsr](https://github.com/breakbadsp/libvsr) does the replication. It
implements Viewstamped Replication: the replicas agree on one ordered log of
operations, and each replica applies the log to its own engines. The engines
are deterministic, so all replicas have the same books.

## How to run a cluster

Add one `replica` line for each replica to the config. All replicas use the
same file.

```
# replica <replication address:port> <gateway port>
replica 10.0.0.1:7000 9000
replica 10.0.0.2:7000 9000
replica 10.0.0.3:7000 9000
```

Start each replica with its number (the order of the `replica` lines, from 0)
and the path of its storage file:

```
mex_exchange exchange.cfg 0 /var/lib/mex/replica0.data
```

The first start makes the storage file. A later start reads it, so a replica
that stopped or crashed continues with its data. Use an odd number of
replicas, at most 7. Without `replica` lines the exchange runs as one process
with no replication, as before.

A client can connect to the gateway of any replica. The protocol does not
change. When its replica fails, the client connects to another replica, logs
on, and sends the requests that have no final report again with the same
request numbers. A request that already executed gets a `DuplicateRequest`
reject and is not executed again.

`tests/replicated_smoke_test.sh` runs three replicas, kills the primary,
starts it again, and compares the state of all three.

## Design

```
client -> Gateway -> inbound queues -> ReplicationBridge -> libvsr cluster
libvsr commit -> ExchangeStateMachine -> engines -> outbound queues -> Gateway -> client
```

Each replica process has three threads:

- `mex-gateway`: the gateway, not changed. It checks each order and queues it.
- `mex-bridge`: `ReplicationBridge`. It packs the queued orders into a batch
  and sends the batch to the cluster as one operation, with the libvsr client.
  The client finds the primary, so the gateway of a backup also serves
  clients.
- `mex-replica`: the libvsr replica. When an operation is committed it calls
  `ExchangeStateMachine`, which applies each order of the batch to the engine
  of its symbol with `Engine::Apply`.

The code is in `src/replication.hpp` and `src/replicated_exchange.cpp`.

### Decisions

- **One operation is a batch of orders, not one order.** libvsr permits one
  request in progress for each of its clients, and each operation costs one
  storage sync. A session sends many orders before the first report. So the
  bridge is the libvsr client, and all orders that arrive while one batch is
  in progress go into the next batch. TigerBeetle batches in the same way.
- **The gateway does not change.** The bridge takes the place of the engine
  threads at the other end of the queues.
- **All engines apply on the replica thread.** The log gives one total order,
  and libvsr calls the state machine from one thread. The engines of one
  replica do not run in parallel. The `core` of an `engine` line is not used
  in a cluster.
- **Reports are an output of each replica, not replicated state.** An order
  can fill any number of resting orders, so its reports have no fixed size
  limit, but a libvsr reply has one. Each replica pushes the reports of each
  operation to its own gateway, and the gateway sends them to the sessions
  that it holds. A maker and a taker can use different replicas.
- **The engine finds a repeated request, not libvsr.** The request number of
  the session is in each order, and the client table of the engine is
  replicated state. A batch that the cluster applies twice would only give
  `DuplicateRequest` rejects.
- **The state machine checks each order again.** An operation can come from
  any process that reaches the replication port. An entry that fails the
  gateway checks is not applied.
- **A snapshot is the state that `Engine::StateDigest` covers.** libvsr takes
  a checkpoint every 256 operations and writes the full snapshot. A replica
  that is far behind gets the snapshot from another replica.

### Engine op numbers

The op number of an order is the libvsr operation number and the index of the
order in its batch, packed in 64 bits. The op counter of the gateway is not
used in a cluster.

## Limits

- **Reports can be lost, and the engine does not send them again.** A report
  is lost if its session has no connection to the replica at that moment, or
  if the replica fails before it sends the report. The order state itself is
  safe. A `DuplicateRequest` reject after a reconnect says only that the
  engine did not execute the request now. It does not carry the result of
  the first attempt. A fill of a resting order that is lost is not sent
  again. "Resend of reports after reconnect"
  in `prep/io-design-spec.md` describes what a fix needs.
- **A replica sends no report for an older operation.** After a start it
  applies the operations of its log and the operations that it missed. It
  pushes reports only for operations that the primary made after this process
  started. The clocks of the replicas decide this, so keep them in step
  (NTP). A clock difference can drop or repeat reports of the first moments
  after a start.
- **Requests can be lost.** If the cluster removes the libvsr session of a
  bridge, the bridge drops the batch in progress and counts its orders
  ("bridge abandoned orders" at exit). The clients get no report and send
  the requests again.
- **A full snapshot at each checkpoint.** The replica thread writes the state
  of all books and waits for the storage sync. The size follows the number of
  resting orders. Large books give a pause at each checkpoint.
- **The libvsr limits apply.** Version 1 handles crashes and power loss with
  a disk that does not corrupt data. It has no reconfiguration: the replica
  list is fixed. See the libvsr `docs/design-decisions.md`.
- **Throughput is lower than one process.** Each batch waits for a storage
  sync on a majority of the replicas.
