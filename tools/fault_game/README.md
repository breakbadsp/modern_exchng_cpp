# MEX fault game

Inject process faults into a local cluster of seven real MEX replicas. A
quorum of four replicas commits orders, so the cluster tolerates three failed
processes. The order client uses the MEX TCP protocol. libvsr performs
replication and recovery.
Python 3 and the built `mex_exchange` binary are required. No Python packages
or frontend build tools are required.

From the repository root:

```sh
cmake --preset ninja
cmake --build --preset ninja
python3 tools/fault_game/server.py
```

Open `http://127.0.0.1:8080`. Use `--port` if that port is occupied. Use
`--exchange /path/to/mex_exchange` for another build. `--rate 30` sets the
maximum order rate; it is not a throughput benchmark. Each game uses new
storage files and 14 available loopback ports (two for each replica).

The page shows the number of running replicas, the quorum, and the next step.
**Crash** kills a process, **Freeze** pauses it, and **Restore** resumes it or
starts it from its storage file. The controls work with the keyboard.

1. Crash or freeze three replicas. Observe order reports while only four
   replicas run.
2. Crash or freeze one more replica. Wait for the client to stop getting
   terminal reports for 3 seconds. Three replicas cannot form a quorum.
3. Restore replicas until at least four run. The client sends an uncertain
   request again with its original request number. A duplicate reject counts separately from an ack.
4. Select **Verify recovered state**. The game pauses the client, restores all
   replicas, and sends an order through each gateway with a separate probe
   session. It stops the replicas cleanly and compares their state digests
   and commit positions. It then starts them with the same storage files.
   If a replica stops before it learns the last commit, the commit positions
   differ and the result is inconclusive, not a failure. Verify again.
   A process or connection error gives an error result. A failed result means
   that state digests differ at the same commit position.

The four checks stay complete after observation. Verification records the
state at the time of the check. Its probe orders are not in the workload
counters. Fill reports count received messages, including maker and taker
reports, rather than unique trades. The replica cards show configured
membership and process state; the UI does not measure replication traffic or
the current primary.

The game uses `SIGKILL` for a crash and `SIGSTOP` for a freeze. Restore uses
`SIGCONT` or starts the replica with its existing storage. It does not test
network partitions or corrupt storage. See [replication limits](../../docs/replication.md#limits),
including reports that cannot be recovered after a disconnect. Equal state
digests check agreement; they do not prove that all possible faults are safe.

Stop the server with Ctrl+C, `kill -INT`, or `kill -TERM`, also when it runs as a
background job. All child processes stop. They also stop when the server gets
`SIGHUP` or `SIGKILL`. Logs and storage stay
in the temporary directory printed at startup. One game uses approximately
540 MB there. Delete that directory after inspection; if the temporary file
system is full, the replicas cannot start and the server stops with the
reason. The service binds only to loopback and controls only its own child
processes.

Run the real-process verification:

```sh
python3 tests/fault_game_test.py
```

The check uses the HTTP API to confirm seven live processes, keep progress
with three replicas down, stop progress with four down, restore progress,
verify seven equal state digests twice, and recover from a crash of all seven
replicas. It also checks gateway retry rotation, invalid actions, and process
cleanup when the server stops with verification queued behind a blocked order
client. If Chromium is installed, it also reads the rendered page in a headless
browser; without Chromium it prints `SKIP` for that part. The check deletes
its storage directory when it passes.

The replica count and the quorum are set one time, at the top of `server.py`.
The page (`index.html`, `game.css`, `game.js`) reads them from `/api/state`.
