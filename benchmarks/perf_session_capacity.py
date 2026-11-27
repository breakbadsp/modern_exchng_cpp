#!/usr/bin/env python3
"""Measure order and reconnect P100 with many authenticated loopback sockets.

Run on the six-core test layout with taskset -c 2. Cores 0–1 run the
load generators, core 2 runs this driver, and cores 3–5 run the exchange.
"""
import asyncio
from contextlib import AsyncExitStack
from pathlib import Path
import re
import socket
import struct
from tempfile import TemporaryDirectory
import time

ACTIVE = 4
OFFERED_RATE = 400_000
ORDERS_PER_CLIENT = 600_000
WINDOW = 4096
LOGIN_BATCH = 256
RECONNECT_MAX = 1024


def configure(count, port):
    lines = ["engine 4", "engine 5"]
    # Separate symbols and sessions let the two phases start with empty books.
    for index in range(2 * ACTIVE):
        lines.extend([
            f"symbol {101 + index} {index % 2} 100000 4096 1 1000000",
            f"session a{index} t",
        ])
    lines.extend(f"session i{index:x} t" for index in range(count - ACTIVE))
    lines.extend([
        "gateway address 127.0.0.1",
        f"gateway port {port}",
        f"gateway max_connections {count + 128}",
        "gateway inbound_queue_capacity 65536",
        "gateway outbound_queue_capacity 65536",
    ])
    return "\n".join(lines) + "\n"


async def read_exact(loop, sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = await loop.sock_recv(sock, size - len(data))
        if not chunk:
            raise ConnectionError("Connection closed before LogonAck")
        data.extend(chunk)
    return data


async def login(port, index):
    loop = asyncio.get_running_loop()
    frame = struct.pack("<HB16s16s", 35, 1, f"i{index:x}".encode(), b"t")
    sock = socket.socket()
    try:
        sock.setblocking(False)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        # Spread ephemeral ports across loopback source addresses.
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_BIND_ADDRESS_NO_PORT, 1)
        sock.bind((f"127.0.0.{2 + index // 8000}", 0))
        started = time.monotonic_ns()
        async with asyncio.timeout(8):
            await loop.sock_connect(sock, ("127.0.0.1", port))
            await loop.sock_sendall(sock, frame)
            reply = await read_exact(loop, sock, 11)
        if struct.unpack("<HBQ", reply) != (11, 64, 1):
            raise ConnectionError(f"Expected LogonAck for session {index}, got {reply.hex()}")
        return sock, (time.monotonic_ns() - started) / 1_000_000
    except BaseException:
        sock.close()
        raise


async def login_group(port, indices, sockets):
    async def open_one(index):
        sockets[index], latency = await login(port, index)
        return latency

    async with asyncio.TaskGroup() as group:
        tasks = [group.create_task(open_one(index)) for index in indices]
    return max((task.result() for task in tasks), default=0.0)


async def terminate(process):
    if process.returncode is None:
        process.terminate()
        try:
            await asyncio.wait_for(process.communicate(), 4)
        except TimeoutError:
            if process.returncode is None:
                process.kill()
            await process.communicate()


async def start(stack, cpus, binary, *args):
    process = await asyncio.create_subprocess_exec(
        "taskset", "-c", cpus, str(binary), *args,
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT,
    )
    stack.push_async_callback(terminate, process)
    return process


def client_metrics(log):
    summary = re.search(r"sent (\d+) operations", log)
    latency = re.search(r"max ([\d.]+) us \((\d+) samples\)", log)
    if (not summary or int(summary[1]) != ORDERS_PER_CLIENT or not latency
            or "Reject SystemBusy 0)" not in log):
        raise RuntimeError(f"Invalid load-generator result:\n{log}")
    return float(latency[1]) / 1000, int(latency[2])


async def trade(build, port, sockets, reconnect_count=0):
    async def completed(process):
        output, _ = await process.communicate()
        if process.returncode:
            raise RuntimeError(output.decode())
        return client_metrics(output.decode()), time.monotonic()

    async def churn():
        await asyncio.sleep(1.5)
        count = min(reconnect_count, len(sockets))
        indices = [index * len(sockets) // count for index in range(count)]
        for index in indices:
            sockets[index].close()
        await asyncio.sleep(0.050)
        return await login_group(port, indices, sockets), count, time.monotonic()

    async with AsyncExitStack() as stack, asyncio.TaskGroup() as group:
        started = time.monotonic()
        offset = ACTIVE if reconnect_count else 0
        clients = []
        for index in range(offset, offset + ACTIVE):
            process = await start(
                stack, "0-1", build / "mex_loadgen", "--port", str(port),
                "--session", f"a{index}", "--token", "t", "--symbols", str(101 + index),
                "--orders", str(ORDERS_PER_CLIENT), "--window", str(WINDOW),
                "--rate", str(OFFERED_RATE // ACTIVE),
            )
            clients.append(group.create_task(completed(process)))
        burst = group.create_task(churn()) if reconnect_count else None

    metrics, times = zip(*(client.result() for client in clients))
    finished = max(times)
    result = {
        "ops_s": round(ACTIVE * ORDERS_PER_CLIENT / (finished - started)),
        "order_p100_ms": round(max(latency for latency, _ in metrics), 4),
        "order_samples": sum(samples for _, samples in metrics),
    }
    if burst is not None:
        latency, count, burst_end = burst.result()
        if burst_end > min(times):
            raise RuntimeError("The reconnect burst outlasted a trading client")
        result.update(reconnect_p100_ms=round(latency, 4), reconnected=count)
    return result


async def case(build, count, reconnect_count):
    # Reserve only until the process can bind the same local listen address.
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]

    sockets = [None] * (count - ACTIVE)
    try:
        with TemporaryDirectory(prefix="mex-session-capacity-") as work:
            config = Path(work) / "exchange.cfg"
            config.write_text(configure(count, port), encoding="ascii")
            async with AsyncExitStack() as stack:
                exchange = await start(stack, "3-5", build / "mex_exchange", str(config))
                async with asyncio.timeout(90):
                    while True:
                        line = await exchange.stdout.readline()
                        if b"listening on" in line:
                            break
                        if not line:
                            raise RuntimeError("Exchange did not start")
                for start_at in range(0, len(sockets), LOGIN_BATCH):
                    indices = range(start_at, min(start_at + LOGIN_BATCH, len(sockets)))
                    await login_group(port, indices, sockets)

                rss_line = next(line for line in Path(f"/proc/{exchange.pid}/status").read_text().splitlines()
                                if line.startswith("VmRSS:"))
                rss_mib = round(int(rss_line.split()[1]) / 1024)
                async with asyncio.timeout(30):
                    steady = await trade(build, port, sockets)
                    reconnect = await trade(build, port, sockets, reconnect_count)
                exchange.terminate()
                output, _ = await asyncio.wait_for(exchange.communicate(), 15)
                log = output.decode()
                if exchange.returncode or any(line + ": 0" not in log for line in (
                    "engine 0 outbound stalls", "engine 1 outbound stalls", "gateway dropped reports",
                )):
                    raise RuntimeError(f"Invalid exchange result:\n{log}")
                return {"sessions": count, "active": ACTIVE, "rss_mib": rss_mib,
                        "steady": steady, "reconnect": reconnect}
    finally:
        for sock in sockets:
            if sock is not None:
                sock.close()
