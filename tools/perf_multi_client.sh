#!/usr/bin/env bash
# Runs N load-generator clients against one exchange and prints one result line.
# Client i logs on as session s<i> and trades only symbol 101+i. Symbols alternate
# between engine 0 (core 4) and engine 1 (core 5). The exchange runs on cores 3-5
# and the clients on cores 0-2, so they do not share a core.
#
# Usage: tools/perf_multi_client.sh <build dir> <clients 1-8> <orders per client> \
#          <window> <rate per client, 0 = closed loop> [epoll|uring]
# The I/O backend argument needs a mex_exchange that accepts --io.
set -euo pipefail

build=$1
clients=$2
orders=$3
window=$4
rate=$5
io=${6:-}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
port=$((20000 + RANDOM % 20000))
{
  echo "engine 4"
  echo "engine 5"
  for i in $(seq 0 7); do echo "symbol $((101 + i)) $((i % 2)) 1000000 4096 1 1000000"; done
  for i in $(seq 0 7); do echo "session s$i token$i"; done
  echo "gateway address 127.0.0.1"
  echo "gateway port $port"
  echo "gateway max_connections 64"
  echo "gateway inbound_queue_capacity 65536"
  echo "gateway outbound_queue_capacity 65536"
} > "$work/exchange.cfg"

io_args=()
if [[ -n "$io" ]]; then
  io_args=(--io "$io")
fi
taskset -c 3-5 "$build/mex_exchange" "$work/exchange.cfg" "${io_args[@]}" > "$work/exchange.log" 2>&1 &
exchange=$!
for _ in $(seq 50); do
  if ss -ltn | grep -q ":$port "; then
    break
  fi
  sleep 0.1
done

start=$(date +%s.%N)
pids=()
for i in $(seq 0 $((clients - 1))); do
  taskset -c 0-2 "$build/mex_loadgen" --port "$port" --session "s$i" --token "token$i" \
    --symbols $((101 + i)) --orders "$orders" --window "$window" --rate "$rate" \
    > "$work/client$i.log" 2>&1 &
  pids+=($!)
done
failed=0
for pid in "${pids[@]}"; do
  wait "$pid" || failed=1
done
end=$(date +%s.%N)
kill -INT "$exchange"
exchange_rc=0
wait "$exchange" || exchange_rc=$?

# Total ops/s uses wall time over all clients. p50 is the median client's p50;
# p99 is the worst client's p99.
python3 - "$work" "$clients" "$start" "$end" "$failed" "$exchange_rc" <<'PY'
import re, sys
work, clients, start, end, failed, exchange_rc = sys.argv[1:]
ops, p50, p99 = 0, [], []
for i in range(int(clients)):
    text = open(f"{work}/client{i}.log").read()
    m = re.search(r"sent (\d+) operations", text)
    ops += int(m.group(1)) if m else 0
    m = re.search(r"p50 ([\d.]+) us, p99 ([\d.]+) us", text)
    if m:
        p50.append(float(m.group(1)))
        p99.append(float(m.group(2)))
wall = float(end) - float(start)
print(f"clients={clients} ops={ops} total_ops_s={ops / wall:,.0f} "
      f"p50_us={sorted(p50)[len(p50) // 2] if p50 else 0:.1f} p99_us={max(p99) if p99 else 0:.1f} "
      f"client_failed={failed} exchange_rc={exchange_rc}")
PY
grep -E 'stalls|dropped' "$work/exchange.log" || true
