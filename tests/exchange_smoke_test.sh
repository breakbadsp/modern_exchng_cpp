#!/usr/bin/env bash
# End-to-end smoke test: starts mex_exchange on a free port, runs mex_loadgen
# against it, then stops the exchange with SIGINT. Both must exit 0.
# Usage: exchange_smoke_test.sh <mex_exchange> <mex_loadgen> <example config> <work dir>
set -euo pipefail

exchange=$1
loadgen=$2
example_config=$3
work_dir=$4
mkdir -p "$work_dir"
config="$work_dir/exchange.cfg"
log="$work_dir/exchange.log"

pid=""
cleanup()
{
  if [[ -n "$pid" ]]; then
    kill -KILL "$pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

# The config needs a fixed port, so try a few random ones in case one is taken.
for _ in 1 2 3 4 5; do
  port=$((20000 + RANDOM % 40000))
  sed "s/^gateway port .*/gateway port $port/" "$example_config" >"$config"
  "$exchange" "$config" >"$log" 2>&1 &
  pid=$!
  for _ in $(seq 100); do
    if grep -q "listening" "$log" || ! kill -0 "$pid" 2>/dev/null; then
      break
    fi
    sleep 0.1
  done
  if grep -q "listening" "$log"; then
    break
  fi
  cat "$log"
  wait "$pid" || true
  pid=""
done
if [[ -z "$pid" ]]; then
  echo "FAIL: the exchange did not start"
  exit 1
fi

# Check the names shown by Linux monitoring tools after all threads start.
expected_names=$(
  {
    printf '%s\n' mex-main mex-gateway
    awk '$1 == "engine" { print "mex-engine-" engine_index++ }' "$config"
  } | sort
)
for _ in $(seq 100); do
  actual_names=$(cat /proc/"$pid"/task/*/comm | sort)
  if [[ "$actual_names" == "$expected_names" ]]; then
    break
  fi
  sleep 0.01
done
if [[ "$actual_names" != "$expected_names" ]]; then
  printf 'FAIL: thread names\nExpected:\n%s\nActual:\n%s\n' "$expected_names" "$actual_names"
  exit 1
fi

loadgen_status=0
"$loadgen" --port "$port" --symbols 1,2,3 --orders 20000 --window 512 || loadgen_status=$?

kill -INT "$pid"
exchange_status=0
wait "$pid" || exchange_status=$?
pid=""
cat "$log"

echo "loadgen exit $loadgen_status, exchange exit $exchange_status"
if [[ $loadgen_status -ne 0 || $exchange_status -ne 0 ]]; then
  echo "FAIL"
  exit 1
fi
echo "PASS"
