#!/usr/bin/env python3
"""Measure a fixed order rate across active MEX sessions on loopback.

Three client workers use CPUs 0–2 and the exchange uses CPUs 3–5.
Every session alternates a resting limit order and its cancel.
"""
import asyncio
import ctypes
from multiprocessing import get_context
import os
from pathlib import Path
import select
import socket
import subprocess
from tempfile import TemporaryDirectory
import time

from perf_session_capacity import ACTIVE, configure, login

SECOND = 1_000_000_000
WORKERS = 3
OPTIONS = ("start_ns", "seconds", "base_rate", "extra_rate", "worker", "hot_symbol")
METRICS = ("base_orders", "burst_orders", "burst_subset", "min_orders", "send_lag_ns",
           "scheduled_ns", "steady_rtt_ns", "burst_rtt_ns", "duration_ns")


class Options(ctypes.Structure):
    _fields_ = [(name, ctypes.c_int64) for name in OPTIONS]


class Metrics(ctypes.Structure):
    _fields_ = [(name, ctypes.c_int64) for name in METRICS]


async def connect(port, indices):
    sockets = []
    for first in range(0, len(indices), 128):
        results = await asyncio.gather(*(login(port, index) for index in indices[first:first + 128]))
        sockets.extend(sock for sock, _ in results)
    return sockets


def work(library, sockets, poll, options):
    loop = ctypes.CDLL(str(library)).MexActiveLoop
    loop.argtypes = (ctypes.POINTER(ctypes.c_int), ctypes.c_size_t, ctypes.c_int,
                     ctypes.POINTER(Options), ctypes.POINTER(Metrics))
    loop.restype = ctypes.c_int
    fds = (ctypes.c_int * len(sockets))(*(sock.fileno() for sock in sockets))
    result = Metrics()
    status = loop(fds, len(sockets), poll.fileno(), ctypes.byref(options), ctypes.byref(result))
    if status:
        error = {1: "unexpected report", 2: "socket I/O failed", 3: "drain deadline expired",
                 4: "missing order or report", 5: "exchange returned SystemBusy",
                 6: "sender in-flight limit reached"}.get(status, status)
        raise RuntimeError(f"Native benchmark loop failed: {error}")
    return {
        "base_orders": result.base_orders, "burst_orders": result.burst_orders,
        "sessions": len(sockets), "burst_subset": result.burst_subset,
        "min_orders_per_session": result.min_orders,
        "sender_p100_ms": result.send_lag_ns / 1e6,
        "scheduled_p100_ms": result.scheduled_ns / 1e6,
        "steady_rtt_p100_ms": result.steady_rtt_ns / 1e6,
        "burst_rtt_p100_ms": result.burst_rtt_ns / 1e6,
        "seconds": result.duration_ns / SECOND,
    }


def worker(pipe, library, port, indices, cpu, seconds, base, extra, hot_symbol):
    try:
        os.sched_setaffinity(0, {cpu})
        sockets = asyncio.run(connect(port, indices))
        with select.epoll() as poll:
            for sock in sockets:
                poll.register(sock, select.EPOLLIN)
            pipe.send(("ready", None))
            start = pipe.recv()
            options = Options(start, seconds, base, extra, cpu, hot_symbol)
            pipe.send(("result", work(library, sockets, poll, options)))
    except Exception as error:
        pipe.send(("error", repr(error)))
    finally:
        pipe.close()


def receive(pipe, phase, timeout):
    if not pipe.poll(timeout):
        raise RuntimeError(f"Client worker timed out waiting for {phase}")
    message, value = pipe.recv()
    if message != phase:
        raise RuntimeError(f"Expected {phase}, got {message}: {value}")
    return value


def clients(library, port, count, seconds, base, extra, hot_symbol):
    context = get_context("fork")
    machines, pipes = [], []
    try:
        for cpu in range(WORKERS):
            parent, child = context.Pipe()
            process = context.Process(target=worker, args=(
                child, library, port, range(cpu, count, WORKERS), cpu, seconds,
                base, extra, hot_symbol,
            ))
            process.start()
            machines.append(process)
            pipes.append(parent)
            child.close()
        for pipe in pipes:
            receive(pipe, "ready", 90)
        start = time.monotonic_ns() + SECOND // 5
        for pipe in pipes:
            pipe.send(start)
        results = [receive(pipe, "result", seconds + 4) for pipe in pipes]
    finally:
        for process in machines:
            process.join(1)
            if process.is_alive():
                process.terminate()
                process.join()
        for pipe in pipes:
            pipe.close()

    totals = {key: sum(result[key] for result in results)
              for key in ("sessions", "base_orders", "burst_orders", "burst_subset")}
    maxima = {key: round(max(result[key] for result in results), 3)
              for key in ("sender_p100_ms", "scheduled_p100_ms", "steady_rtt_p100_ms",
                          "burst_rtt_p100_ms")}
    ops_s = (totals["base_orders"] + totals["burst_orders"]) / max(r["seconds"] for r in results)
    return {**totals, **maxima, "ops_s": round(ops_s),
            "min_orders_per_session": min(r["min_orders_per_session"] for r in results)}


def case(build, library, count, seconds, base, extra, hot_symbol=False, io="epoll"):
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    with TemporaryDirectory(prefix="mex-active-") as work:
        config = Path(work) / "exchange.cfg"
        config.write_text(configure(count + ACTIVE, port), encoding="ascii")
        with subprocess.Popen(
            ["taskset", "-c", "3-5", str(build / "mex_exchange"), str(config), "--io", io],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        ) as exchange:
            try:
                readiness = select.poll()
                readiness.register(exchange.stdout, select.POLLIN)
                if not readiness.poll(90_000) or "listening on" not in exchange.stdout.readline():
                    raise RuntimeError("Exchange did not start")
                result = clients(library, port, count, seconds, base, extra, hot_symbol)
                status = Path(f"/proc/{exchange.pid}/status").read_text().splitlines()
                peak = next(line for line in status if line.startswith("VmHWM:"))
                result["rss_mib"] = round(int(peak.split()[1]) / 1024)
            finally:
                if exchange.poll() is None:
                    exchange.terminate()
                try:
                    log, _ = exchange.communicate(timeout=15)
                except subprocess.TimeoutExpired:
                    exchange.kill()
                    exchange.communicate()
                    raise
        if exchange.returncode or any(label + ": 0" not in log for label in (
            "engine 0 outbound stalls", "engine 1 outbound stalls", "gateway dropped reports",
        )):
            raise RuntimeError(f"Exchange reported stalls or drops:\n{log}")
    return result


def compile_loop(library):
    repo = Path(__file__).resolve().parent.parent
    subprocess.run(
        [os.environ.get("CXX", "g++"), "-std=c++26", "-fcontracts", "-O3", "-DNDEBUG",
         "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", f"-I{repo / 'src'}",
         str(repo / "benchmarks/perf_active_loop.cpp"), "-o", str(library)],
        check=True,
    )
