# MEX performance tests

Build the exchange and load generator before you start. Run on Linux with
CPUs 0–5 available. The client workers use CPUs 0–2. The exchange uses
CPUs 3–5, with its two engines pinned to CPUs 4 and 5. Run one suite at a
time. Use a quiet host when you need comparable P100 results.

```sh
cmake --preset ninja
cmake --build --preset ninja
python3 -B benchmarks/run.py --suite smoke --seconds 1 --orders 100000 --out /tmp/mex-smoke
python3 -B benchmarks/run.py --suite realistic --repeat 3 --out /tmp/mex-realistic
python3 -B benchmarks/run.py --suite stress --repeat 3 --out /tmp/mex-stress
```

Open `report.html` in a browser. Keep `results.json` for the environment,
limits, and all measurements. The runner writes each result to `runs.jsonl`
as it finishes, so completed runs remain available if the suite stops.
Use a new output directory for each run.

## Workloads

| Scenario | What it drives |
| --- | --- |
| Active | Every authenticated session alternates a resting limit buy and its cancel. The base load visits all sessions in turn. A rotating subset of about 5% sends extra orders during a 200 ms interval each second. Two symbols split the work across two engines. |
| Hot symbol | Every session uses the same symbol. This puts all matching work on one engine. |
| Reconnect | Four trading sessions offer 400k ops/s in total. Other sessions stay logged on. One cohort disconnects and attempts to reconnect after 50 ms. The test records order P100 and TCP-connect-to-LogonAck P100 separately. |
| Saturation | One to eight load generators send a NewOrder, Cancel, and Replace mix as fast as their outstanding windows allow. The throughput includes process launch and report drain. |

The stress suite tests larger connection counts, higher base rates, and
several closed-loop windows. To test one dimension, use a custom matrix.
For active workloads, `--burst 0` disables bursts. A positive burst rate
must be at least 5 ops/s so each 200 ms interval can contain an extra
operation. For reconnect workloads, the cohort must fit within the
logged-on idle sessions.

```sh
python3 -B benchmarks/run.py --scenario active --sessions 1024 4096 8192 \
  --rates 100000 200000 400000 --burst 100000 --repeat 3 --out /tmp/mex-rate-sweep
python3 -B benchmarks/run.py --scenario reconnect --sessions 65536 --burst 512 \
  --out /tmp/mex-reconnect
python3 -B benchmarks/run.py --scenario saturation --sessions 1 8 --windows 64 512 4096 \
  --io epoll --out /tmp/mex-saturation
```

## Interpret the limits

The default target is a maximum observed latency below **100 ms**. Use
`--target-ms` to change it. A setting passes only when every repeat passes.
The report gives the largest passing settings tested and keeps invalid runs
visible. An invalid run can indicate failed logon, a socket error, a
rejected or missing report, or an engine stall.

For the paced workloads, the delivered rate must be at least 98% of the
average offered rate. The active driver also gets at most one quarter
of the target as send lag. A larger lag makes the run driver-limited.
This makes it possible to see when the test host or client cannot
generate the intended traffic. The JSON has both send-to-report RTT and
scheduled-send-to-report P100 for the active cases. Scheduled P100
includes client send lag and uses all NewOrder and Cancel replies.
The legacy load generator reports NewOrder-to-first-report P100.

These are standalone, local TCP tests. The active scenarios use resting
orders and cancels. The saturation mix also exercises matches.
Record network and replication behavior with separate tests. The default
backend is `epoll`. Use `--io uring` for an independent run on a host that
supports it. The active runner builds its native timed loop with GCC 16
and uses the exchange wire codec. All sockets connect to loopback.
