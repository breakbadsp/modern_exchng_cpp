#!/usr/bin/env python3
"""Run MEX loopback benchmarks and write JSON and HTML reports."""
import argparse
import asyncio
from dataclasses import dataclass
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import re
import resource
import subprocess
import sys
from tempfile import TemporaryDirectory

import perf_active_sessions as active
import perf_session_capacity as reconnect
from report import DRIVER_BUDGET_FRACTION, MIN_RATE_FRACTION, classify, write_report

REPO = Path(__file__).resolve().parent.parent
SCENARIOS = ("active", "hot-symbol", "reconnect", "saturation")
MAX_SESSIONS = 65_536


@dataclass(frozen=True)
class Case:
    scenario: str
    sessions: int
    base: int = 0
    burst: int = 0
    window: int = 0


def smoke():
    return [
        Case("active", 64, 100_000, 25_000),
        Case("hot-symbol", 64, 100_000, 25_000),
        Case("reconnect", 64, 400_000, 60),
        Case("saturation", 1, window=64),
    ]


def realistic():
    return [
        *(Case("active", count, 400_000, 100_000) for count in (256, 1024, 4096, 8192)),
        *(Case("hot-symbol", count, 400_000, 100_000) for count in (1024, 4096)),
        *(Case("reconnect", count, 400_000, 512) for count in (1024, 8192)),
    ]


def stress():
    return [
        *(Case("active", count, 200_000, 50_000) for count in (4096, 8192, 16384, 32768, 65536)),
        *(Case("active", 4096, rate, 0) for rate in (100_000, 200_000, 400_000, 600_000, 800_000)),
        *(Case("reconnect", count, 400_000, burst) for count in (8192, 65536) for burst in (512, 1024)),
        *(Case("saturation", count, window=window) for count in (1, 2, 4, 8)
          for window in (64, 512, 4096)),
    ]


def requested_cases(args):
    if args.suite:
        return {"smoke": smoke, "realistic": realistic, "stress": stress}[args.suite]()
    counts = args.sessions or ([4] if args.scenario == "saturation" else [1024])
    if args.scenario == "saturation":
        return [Case(args.scenario, count, window=window)
                for count in counts for window in (args.windows or [64, 512, 4096])]
    if args.scenario == "reconnect":
        return [Case("reconnect", count, 400_000, args.burst if args.burst is not None else min(512, count - reconnect.ACTIVE))
                for count in counts]
    return [Case(args.scenario, count, base, args.burst if args.burst is not None else 100_000)
            for count in counts for base in (args.rates or [400_000])]


def saturation(build, case, orders, io):
    result = subprocess.run(
        ["timeout", "--kill-after=5", "60", str(REPO / "tools/perf_multi_client.sh"),
         str(build), str(case.sessions), str(orders), str(case.window), "0", io],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    log = result.stdout
    if result.returncode or log.count("outbound stalls: 0") != 2 or "dropped reports: 0" not in log:
        raise RuntimeError(log)
    match = re.search(
        r"ops=(\d+) total_ops_s=([\d,]+).*p100_us=([\d.]+) samples=(\d+) system_busy=(\d+)", log
    )
    if not match or int(match[1]) != orders * case.sessions or not int(match[4]):
        raise RuntimeError(f"Incomplete saturation result\n{log}")
    return {"ops_s": int(match[2].replace(",", "")), "order_p100_ms": float(match[3]) / 1000,
            "samples": int(match[4]), "operations": int(match[1]), "system_busy": int(match[5])}


def reconnect_case(build, case, io):
    affinity = os.sched_getaffinity(0)
    try:
        os.sched_setaffinity(0, {2})
        result = asyncio.run(reconnect.case(build, case.sessions, case.burst, io))
    finally:
        os.sched_setaffinity(0, affinity)
    steady, churn = result["steady"], result["reconnect"]
    if churn["reconnected"] != case.burst:
        raise RuntimeError(f"Expected {case.burst} reconnects, observed {churn['reconnected']}")
    return {"offered_ops_s": case.base, "ops_s": min(steady["ops_s"], churn["ops_s"]),
            "order_p100_ms": max(steady["order_p100_ms"], churn["order_p100_ms"]),
            "logon_p100_ms": churn["reconnect_p100_ms"], **result}


def run_case(args, library, case):
    if case.scenario == "saturation":
        return saturation(args.build, case, args.orders, args.io)
    if case.scenario == "reconnect":
        return reconnect_case(args.build, case, args.io)
    result = active.case(args.build, library, case.sessions, args.seconds, case.base, case.burst,
                         hot_symbol=case.scenario == "hot-symbol", io=args.io)
    expected_base = case.base * args.seconds
    expected_burst = args.seconds * (case.burst // 5)
    if (result["sessions"] != case.sessions or result["base_orders"] != expected_base
            or result["burst_orders"] != expected_burst or result["min_orders_per_session"] < 1):
        raise RuntimeError(f"Incomplete active run: {result}")
    if expected_burst == 0:
        result.pop("burst_rtt_p100_ms")
    return {"offered_ops_s": (expected_base + expected_burst) / args.seconds,
            "order_p100_ms": result["scheduled_p100_ms"], **result}


def system_file(path):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return None


def environment(args):
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip()
    dirty = bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=REPO, text=True).strip())
    cpu = system_file("/proc/cpuinfo") or ""
    model = re.search(r"^model name\s*:\s*(.+)$", cpu, re.M)
    return {
        "started_utc": datetime.now(timezone.utc).isoformat(), "source_commit": revision,
        "worktree_dirty": dirty, "io": args.io, "platform": platform.platform(),
        "cpu": model[1] if model else None, "cpu_affinity": sorted(os.sched_getaffinity(0)),
        "memory_kib": os.sysconf("SC_PHYS_PAGES") * os.sysconf("SC_PAGE_SIZE") // 1024,
        "file_descriptor_limits": resource.getrlimit(resource.RLIMIT_NOFILE),
        "listen_backlog": system_file("/proc/sys/net/core/somaxconn"),
        "ephemeral_port_range": system_file("/proc/sys/net/ipv4/ip_local_port_range"),
        "transport": "TCP loopback", "engines": 2, "seconds_per_active_case": args.seconds,
        "orders_per_saturation_client": args.orders, "sets": "client CPUs 0–2, exchange CPUs 3–5",
        "active_window": "200 ms burst per second starting 400 ms in; a rotating ~5% of sessions",
        "min_delivered_rate_fraction": MIN_RATE_FRACTION,
        "sender_lag_budget_fraction_of_p100": DRIVER_BUDGET_FRACTION,
    }


def validate(args, cases, parser):
    if not {0, 1, 2, 3, 4, 5}.issubset(os.sched_getaffinity(0)):
        parser.error("The local CPU layout requires CPUs 0–5.")
    if not 1 <= args.repeat <= 50 or not 1 <= args.seconds <= 300 or not 0 < args.target_ms <= 10000:
        parser.error("Use 1–50 repeats, 1–300 seconds, and a target above 0 and at most 10000 ms.")
    if not 1 <= args.orders <= 16_777_216:
        parser.error("Use 1–16777216 orders per saturation client.")
    for case in cases:
        if case.scenario == "saturation":
            valid = 1 <= case.sessions <= 8 and 1 <= case.window <= args.orders
        elif case.scenario == "reconnect":
            valid = (reconnect.ACTIVE < case.sessions <= MAX_SESSIONS
                     and 1 <= case.burst <= min(1024, case.sessions - reconnect.ACTIVE))
        else:
            valid = (3 <= case.sessions <= MAX_SESSIONS and 1 <= case.base <= 2_000_000
                     and (case.burst == 0 or 5 <= case.burst <= 2_000_000)
                     and case.base * args.seconds >= case.sessions)
        if not valid:
            parser.error(f"Invalid benchmark setting: {case}")
    if not (args.build / "mex_exchange").is_file():
        parser.error(f"Build mex_exchange first in {args.build}.")
    if any(case.scenario in ("reconnect", "saturation") for case in cases):
        if not (args.build / "mex_loadgen").is_file():
            parser.error(f"Build mex_loadgen first in {args.build}.")


def record_case(args, library, case, repeat):
    row = {"scenario": case.scenario, "sessions": case.sessions, "base": case.base,
           "burst": case.burst, "window": case.window, "repeat": repeat}
    try:
        row.update(run_case(args, library, case), status="complete")
    except (RuntimeError, OSError, TimeoutError, subprocess.TimeoutExpired, ExceptionGroup) as exc:
        row.update(status="invalid", error=str(exc))
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--suite", choices=("smoke", "realistic", "stress"))
    mode.add_argument("--scenario", choices=SCENARIOS)
    parser.add_argument("--build", type=Path, default=REPO / "build")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--sessions", type=int, nargs="+")
    parser.add_argument("--rates", type=int, nargs="+")
    parser.add_argument("--windows", type=int, nargs="+")
    parser.add_argument("--burst", type=int, help="Extra ops/s, or a reconnect cohort size.")
    parser.add_argument("--seconds", type=int, default=6)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--orders", type=int, default=1_000_000)
    parser.add_argument("--target-ms", type=float, default=100)
    parser.add_argument("--io", choices=("epoll", "uring"), default="epoll")
    args = parser.parse_args()
    if args.suite and any(value is not None for value in (args.sessions, args.rates, args.windows, args.burst)):
        parser.error("Use --scenario to set sessions, rates, windows, or burst size.")
    if args.windows and args.scenario != "saturation":
        parser.error("--windows applies only to saturation.")
    if args.rates and args.scenario not in ("active", "hot-symbol"):
        parser.error("--rates applies only to active and hot-symbol.")
    if args.burst is not None and args.scenario == "saturation":
        parser.error("--burst does not apply to saturation.")
    args.build = args.build.resolve()
    cases = requested_cases(args)
    validate(args, cases, parser)
    args.out.mkdir(parents=True, exist_ok=True)
    if (args.out / "runs.jsonl").exists():
        parser.error("Use a new output directory to preserve the previous run journal.")
    metadata = environment(args)
    runs = []
    with TemporaryDirectory(prefix="mex-active-loop-") as temp, (
        args.out / "runs.jsonl"
    ).open("w", encoding="utf-8") as journal:
        library = Path(temp) / "loop.so"
        if any(case.scenario in ("active", "hot-symbol") for case in cases):
            active.compile_loop(library)
        for repeat in range(1, args.repeat + 1):
            for case in cases:
                print(f"Run {repeat} · {case.scenario} · {case.sessions:,} sessions", file=sys.stderr)
                row = record_case(args, library, case, repeat)
                row["classification"] = classify(row, args.target_ms)
                runs.append(row)
                print(json.dumps(row), file=journal, flush=True)
                print(json.dumps(row), flush=True)
    print(f"Report: {write_report(args.out, metadata, runs, args.target_ms)}", file=sys.stderr)
    return 0 if all(row["status"] == "complete" for row in runs) else 1


if __name__ == "__main__":
    raise SystemExit(main())
