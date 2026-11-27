"""Drive the trading socket, market feed, and HTTP client as separate processes."""

import contextlib
import json
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        piece = sock.recv(size - len(data))
        if not piece:
            raise EOFError("trading socket closed")
        data.extend(piece)
    return data


class Trader:
    def __init__(self, sock):
        self.sock = sock
        self.request = 0
        self.send(1, struct.pack("<16s16s", b"alice", b"alice-secret"), 64)

    def send(self, kind, body, expected):
        self.sock.sendall(struct.pack("<HB", 3 + len(body), kind) + body)
        for _ in range(128):
            length, response = struct.unpack("<HB", read_exact(self.sock, 3))
            read_exact(self.sock, length - 3)
            if response != 68:  # Fill. The terminal report follows.
                if response != expected:
                    raise AssertionError((expected, response))
                return
        raise AssertionError("missing terminal report")

    def new(self, order, price, qty, *, side=0, kind=0, symbol=1, expected=66):
        self.request += 1
        self.send(2, struct.pack("<QQIBBqI", self.request, order, symbol, side, kind,
                                 price, qty), expected)

    def cancel(self, order, symbol=1):
        self.request += 1
        self.send(3, struct.pack("<QQI", self.request, order, symbol), 69)

    def replace(self, order, price, qty):
        self.request += 1
        self.send(4, struct.pack("<QQIqI", self.request, order, 1, price, qty), 71)


def start(stack, args, pattern):
    log = stack.enter_context(tempfile.TemporaryFile())
    process = subprocess.Popen(args, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)

    def stop():
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    stack.callback(stop)
    end = time.monotonic() + 8
    while time.monotonic() < end:
        output = os.pread(log.fileno(), 65536, 0).decode(errors="replace")
        match = re.search(pattern, output)
        if match:
            return process, tuple(int(port) for port in match.groups())
        if process.poll() is not None:
            break
        time.sleep(0.02)
    raise AssertionError(f"process did not become ready: {args}\n{output}")


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def feed(stack, port):
    sock = stack.enter_context(socket.create_connection(("127.0.0.1", port), timeout=4))
    return stack.enter_context(sock.makefile("rb"))


def message(stream, kind, symbol=None, predicate=lambda _: True):
    kinds = (kind,) if isinstance(kind, str) else kind
    end = time.monotonic() + 4
    for _ in range(512):
        if time.monotonic() > end:
            break
        line = stream.readline()
        if not line:
            raise EOFError(f"feed closed before {kind}")
        item = json.loads(line)
        if (item["type"] in kinds and (symbol is None or item["symbol"] == symbol)
                and predicate(item)):
            return item
    raise AssertionError(f"missing {kind} for {symbol}")


def sse(response, kind, predicate=lambda _: True):
    end = time.monotonic() + 6
    for _ in range(512):
        if time.monotonic() > end:
            break
        line = response.readline()
        if not line:
            raise EOFError(f"SSE closed before {kind}")
        if line == f"event: {kind}\n".encode():
            data = json.loads(response.readline().removeprefix(b"data: "))
            if predicate(data):
                return data
    raise AssertionError(f"missing SSE event {kind}")


def start_exchange(stack, config=ROOT / "config/example.cfg", data_path=None, market_port=0, replica=0):
    args = [EXCHANGE, str(config), "--market-data-port", str(market_port)]
    if data_path is not None:
        args += [str(replica), str(data_path)]
    return start(stack, args, r"market data on [\d.]+:(\d+)[\s\S]*?listening on [\d.]+:(\d+)")


class MarketDataTest(unittest.TestCase):
    def test_live_depth_and_late_join(self):
        """Check observable level updates and reconstruction at the depth boundary."""
        with contextlib.ExitStack() as stack, tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "exchange.cfg"
            config.write_text((ROOT / "config/example.cfg").read_text().replace(
                "gateway port 9000", f"gateway port {free_port()}"))
            _, (market_port, trade_port) = start_exchange(stack, config)
            stream = feed(stack, market_port)
            self.assertEqual(json.loads(stream.readline()), {"type": "hello", "version": 1,
                                                             "depth": 20, "symbols": [1, 2, 3]})
            self.assertEqual(message(stream, "snapshot", 1),
                             {"type": "snapshot", "symbol": 1, "seq": "0", "bids": [], "asks": []})
            sock = stack.enter_context(socket.create_connection(("127.0.0.1", trade_port), 4))
            trader = Trader(sock)
            trader.new(1, 100, 4)
            self.assertEqual(message(stream, "update", 1),
                             {"type": "update", "symbol": 1, "seq": "1",
                              "bids": [["100", "4"]], "asks": []})
            trader.new(2, 100, 6)
            self.assertEqual(message(stream, "update", 1)["bids"], [["100", "10"]])
            trader.new(3, 100, 5, side=1)
            self.assertEqual(message(stream, "update", 1)["bids"], [["100", "5"]])
            trader.replace(2, 100, 4)
            self.assertEqual(message(stream, "update", 1)["bids"], [["100", "3"]])
            trader.replace(2, 99, 7)
            self.assertEqual(dict(message(stream, "update", 1)["bids"]), {"100": "0", "99": "6"})
            for price in range(101, 122):
                trader.new(price, price, 1)
            trader.new(300, 140, 2, side=1)
            trader.new(301, 130, 3, side=1)
            trader.new(400, 88, 5, symbol=2)
            message(stream, "update", 2)
            joined = feed(stack, market_port)
            snapshot = message(joined, "snapshot", 1)
            self.assertEqual(snapshot["bids"], [[str(p), "1"] for p in range(121, 101, -1)])
            self.assertEqual(snapshot["asks"], [["130", "3"], ["140", "2"]])
            trader.cancel(121)
            delta = message(joined, "update", 1)
            self.assertEqual(int(delta["seq"]), int(snapshot["seq"]) + 1)
            self.assertEqual(dict(delta["bids"]), {"121": "0", "101": "1"})
            trader.new(302, 140, 10)
            delta = message(joined, "update", 1)
            self.assertEqual(int(delta["seq"]), int(snapshot["seq"]) + 2)
            self.assertEqual(dict(delta["asks"]), {"130": "0", "140": "0"})
            self.assertIn(["140", "5"], delta["bids"])

    def test_gap_requires_new_snapshot(self):
        """A client must discard its connection state after a sequence gap."""
        sys.path.insert(0, str(ROOT / "tools/market_data"))
        from client import FeedError, Stream

        stream = Stream({"type": "hello", "version": 1, "depth": 20, "symbols": [1]})
        snapshot = {"type": "snapshot", "symbol": 1, "seq": "5",
                    "bids": [["9007199254740993", "10"]], "asks": []}
        self.assertEqual(stream.apply(snapshot)["bids"], snapshot["bids"])
        update = {"type": "update", "symbol": 1, "seq": "6",
                  "bids": [["9007199254740993", "0"], ["100", "20"]], "asks": []}
        self.assertEqual(stream.apply(update)["bids"], [["100", "20"]])
        with self.assertRaises(FeedError):
            stream.apply(dict(update, seq="8"))
        with self.assertRaises(FeedError):
            stream.apply(dict(update, seq="7"))

    def test_replica_snapshot_after_restart(self):
        """A restarted replica catches up and offers a fresh snapshot of the committed book."""
        with contextlib.ExitStack() as stack, tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "replica.cfg"
            ports = set()
            for _ in range(100):
                ports.add(free_port())
                if len(ports) == 6:
                    break
            self.assertEqual(len(ports), 6)
            trade_ports, replica_ports = list(ports)[:3], list(ports)[3:]
            config.write_text((ROOT / "config/example.cfg").read_text() + "\n" +
                              "".join(f"replica 127.0.0.1:{replica} {trade}\n"
                                      for replica, trade in zip(replica_ports, trade_ports)))
            data = Path(directory) / "replica0.db"
            exchange, (market_port, trade_port) = start_exchange(stack, config, data)
            for replica in (1, 2):
                start_exchange(stack, config, Path(directory) / f"replica{replica}.db",
                               replica=replica)
            stream = feed(stack, market_port)
            sock = stack.enter_context(socket.create_connection(("127.0.0.1", trade_port), 4))
            Trader(sock).new(1, 100, 4)
            self.assertEqual(message(stream, "update", 1)["bids"], [["100", "4"]])
            exchange.terminate()
            self.assertEqual(exchange.wait(timeout=5), 0)
            _, (market_port, _) = start_exchange(stack, config, data)
            message(feed(stack, market_port), ("snapshot", "update"), 1,
                    lambda item: item["bids"] == [["100", "4"]])
            recovered = message(feed(stack, market_port), "snapshot", 1)
            self.assertEqual(recovered["bids"], [["100", "4"]])

    def test_browser_stream_recovers_after_restart(self):
        """Check the page, live updates, stale state and a fresh book after restart."""
        with contextlib.ExitStack() as stack, tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "exchange.cfg"
            config.write_text((ROOT / "config/example.cfg").read_text().replace(
                "gateway port 9000", f"gateway port {free_port()}"))
            exchange, (market_port, trade_port) = start_exchange(stack, config)
            _, (http_port,) = start(stack,
                [sys.executable, "tools/market_data/client.py", "--feed-port", str(market_port),
                 "--port", "0"], r"Order book at http://127\.0\.0\.1:(\d+)")
            origin = f"http://127.0.0.1:{http_port}"
            with urllib.request.urlopen(origin, timeout=4) as page:
                self.assertIn(b"Order book", page.read())
            browser = stack.enter_context(urllib.request.urlopen(origin + "/events", timeout=4))
            sse(browser, "book", lambda event: event["symbol"] == 1)
            sock = stack.enter_context(socket.create_connection(("127.0.0.1", trade_port), 4))
            Trader(sock).new(1, 100, 4)
            book = sse(browser, "book", lambda event: event["symbol"] == 1 and event["bids"])
            self.assertEqual(book["bids"], [["100", "4"]])
            exchange.terminate()
            self.assertEqual(exchange.wait(timeout=5), 0)
            self.assertEqual(sse(browser, "status", lambda item: item["state"] == "reconnecting"),
                             {"state": "reconnecting"})
            start_exchange(stack, config, market_port=market_port)
            recovered = sse(browser, "book", lambda event: event["symbol"] == 1)
            self.assertEqual((recovered["seq"], recovered["bids"]), ("0", []))


if __name__ == "__main__":
    EXCHANGE = sys.argv.pop(1)
    unittest.main()
