#!/usr/bin/env bash
# End-to-end test of the replicated exchange: three replicas on loopback.
#   1. A client trades through the gateway of a backup.
#   2. The primary is killed. A client trades through another replica.
#   3. The killed replica starts again and catches up. A client trades
#      through it.
#   4. All replicas stop with SIGINT and must report the same state digest.
# Usage: replicated_smoke_test.sh <mex_exchange> <mex_loadgen> <example config> <work dir>
set -euo pipefail

exchange=$1
loadgen=$2
example_config=$3
work_dir=$4
rm -rf "$work_dir"
mkdir -p "$work_dir"
config="$work_dir/exchange.cfg"
orders=20000

pids=("" "" "")
cleanup()
{
  for pid in "${pids[@]}"; do
    if [[ -n "$pid" ]]; then
      kill -KILL "$pid" 2>/dev/null || true
    fi
  done
}
trap cleanup EXIT

fail()
{
  echo "FAIL: $1"
  for index in 0 1 2; do
    echo "--- replica $index"
    cat "$work_dir/replica$index.log" 2>/dev/null || true
  done
  exit 1
}

start_replica()
{
  local index=$1
  "$exchange" "$config" "$index" "$work_dir/replica$index.data" \
    >>"$work_dir/replica$index.log" 2>&1 &
  pids[index]=$!
}

# Waits until the log of a replica has p_count "listening" lines.
wait_listening()
{
  local index=$1
  local count=$2
  for _ in $(seq 100); do
    if [[ $(grep -c "listening" "$work_dir/replica$index.log" || true) -ge $count ]]; then
      return 0
    fi
    if ! kill -0 "${pids[index]}" 2>/dev/null; then
      return 1
    fi
    sleep 0.1
  done
  return 1
}

run_loadgen()
{
  local port=$1
  local session=$2
  local symbol=$3
  local count=$4
  "$loadgen" --port "$port" --session "$session" --token "$session-secret" \
    --symbols "$symbol" --orders "$count" --window 32 ||
    fail "loadgen of $session through port $port"
}

# Six fixed ports, so try a few random bases in case one is taken.
started=0
for _ in 1 2 3 4 5; do
  base=$((20000 + RANDOM % 40000))
  {
    grep -v "^session " "$example_config"
    echo "session alice alice-secret"
    echo "session bob bob-secret"
    echo "session carol carol-secret"
    for index in 0 1 2; do
      echo "replica 127.0.0.1:$((base + index)) $((base + 10 + index))"
    done
  } >"$config"
  for index in 0 1 2; do
    : >"$work_dir/replica$index.log"
    rm -f "$work_dir/replica$index.data"
    start_replica "$index"
  done
  started=1
  for index in 0 1 2; do
    if ! wait_listening "$index" 1; then
      started=0
    fi
  done
  if [[ $started -eq 1 ]]; then
    break
  fi
  cleanup
  pids=("" "" "")
done
if [[ $started -ne 1 ]]; then
  fail "the replicas did not start"
fi
gateway_port() { echo $((base + 10 + $1)); }

# 1. Replica 0 is the first primary. Trade through the gateway of a backup.
run_loadgen "$(gateway_port 1)" alice 1 "$orders"

# 2. Kill the primary without notice. The other two elect a new primary.
kill -KILL "${pids[0]}"
wait "${pids[0]}" 2>/dev/null || true
pids[0]=""
# More operations than the log holds, so replica 0 needs a snapshot later.
run_loadgen "$(gateway_port 2)" bob 2 $((2 * orders))

# 3. The killed replica starts from its storage file and catches up.
start_replica 0
wait_listening 0 2 || fail "replica 0 did not start again"
run_loadgen "$(gateway_port 0)" carol 3 "$orders"

# 4. Let the backups learn the last commit, then stop all replicas.
sleep 2
for index in 0 1 2; do
  kill -INT "${pids[index]}"
done
for index in 0 1 2; do
  status=0
  wait "${pids[index]}" || status=$?
  pids[index]=""
  if [[ $status -ne 0 ]]; then
    fail "replica $index exit $status"
  fi
done

digests=$(grep -h "state digest" "$work_dir"/replica?.log | tail -n 3 | sort -u)
echo "$digests"
if [[ $(echo "$digests" | wc -l) -ne 1 ]]; then
  fail "the replicas do not agree on the state"
fi
if ! echo "$digests" | grep -q "at operation [1-9]"; then
  fail "no operation was committed"
fi
echo "PASS"
