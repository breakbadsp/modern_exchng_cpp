#!/usr/bin/env python3
"""Reconstruct a MEX depth feed and serve the current books over HTTP."""

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import re
import socket
from threading import BoundedSemaphore, Condition, Thread
import time

ASSETS = Path(__file__).resolve().parent
MAX_LINE_BYTES = 8192
MAX_SYMBOLS = 256
MAX_DEPTH = 20
MAX_HTTP_CLIENTS = 32
IO_TIMEOUT_SECONDS = 4
RECONNECT_SECONDS = 1
BROWSER_HEARTBEAT_SECONDS = 2
UINT64_MAX = (1 << 64) - 1
INT64_MIN = -(1 << 63)
INT64_MAX = (1 << 63) - 1
DECIMAL = re.compile(r"(?:0|-?[1-9][0-9]{0,19})", re.ASCII)


class FeedError(ValueError):
    """The feed needs a fresh connection."""


def integer(value, minimum, maximum):
    if not isinstance(value, str) or not DECIMAL.fullmatch(value):
        raise FeedError("invalid decimal field")
    result = int(value)
    if not minimum <= result <= maximum:
        raise FeedError("decimal field out of range")
    return result


def read_message(stream):
    line = stream.readline(MAX_LINE_BYTES + 1)
    if not line or len(line) > MAX_LINE_BYTES or not line.endswith(b"\n"):
        raise FeedError("closed connection or invalid frame length")
    item = json.loads(line)
    if not isinstance(item, dict):
        raise FeedError("expected a message object")
    return item


def levels(entries, depth, previous, *, snapshot):
    limit = depth if snapshot else 2 * depth
    if not isinstance(entries, list) or len(entries) > limit:
        raise FeedError("invalid level count")
    result = previous.copy()
    seen = set()
    for entry in entries:
        if not isinstance(entry, list) or len(entry) != 2:
            raise FeedError("invalid level")
        price = integer(entry[0], INT64_MIN, INT64_MAX)
        quantity = integer(entry[1], 1 if snapshot else 0, UINT64_MAX)
        if price in seen or (quantity == 0 and price not in previous):
            raise FeedError("invalid price change")
        seen.add(price)
        if quantity == 0:
            del result[price]
        else:
            result[price] = quantity
    if len(result) > depth:
        raise FeedError("book exceeds published depth")
    return result


class Stream:
    """Own the sequence and price levels for one TCP connection."""

    def __init__(self, hello):
        symbols = hello.get("symbols")
        depth = hello.get("depth")
        if (hello.get("type") != "hello" or type(hello.get("version")) is not int
                or hello["version"] != 1 or type(depth) is not int
                or not 1 <= depth <= MAX_DEPTH or not isinstance(symbols, list)
                or not 1 <= len(symbols) <= MAX_SYMBOLS
                or any(type(symbol) is not int or not 0 <= symbol <= 0xffffffff
                       for symbol in symbols)
                or len(set(symbols)) != len(symbols)):
            raise FeedError("unsupported feed header")
        self.symbols = frozenset(symbols)
        self.depth = depth
        self.books = {}

    def apply(self, item):
        if self.books is None:
            raise FeedError("a new connection is required")
        try:
            return self._apply(item)
        except FeedError:
            self.books = None
            raise

    def _apply(self, item):
        kind = item.get("type")
        if kind == "heartbeat" and len(self.books) == len(self.symbols):
            return None
        symbol = item.get("symbol")
        if type(symbol) is not int or symbol not in self.symbols:
            raise FeedError("unknown symbol")
        sequence = integer(item.get("seq"), 0, UINT64_MAX)
        if kind == "snapshot" and symbol not in self.books:
            before_bids, before_asks = {}, {}
        elif kind == "update" and len(self.books) == len(self.symbols):
            previous, before_bids, before_asks = self.books[symbol]
            if sequence != previous + 1:
                raise FeedError("sequence gap")
        else:
            raise FeedError("unexpected message before or after snapshot")
        bids = levels(item.get("bids"), self.depth, before_bids, snapshot=kind == "snapshot")
        asks = levels(item.get("asks"), self.depth, before_asks, snapshot=kind == "snapshot")
        if bids and asks and max(bids) >= min(asks):
            raise FeedError("crossed book")
        self.books[symbol] = (sequence, bids, asks)
        return {"symbol": symbol, "seq": str(sequence),
                "bids": [[str(p), str(bids[p])] for p in sorted(bids, reverse=True)],
                "asks": [[str(p), str(asks[p])] for p in sorted(asks)]}


def event(kind, payload):
    return f"event: {kind}\ndata: {json.dumps(payload, separators=(',', ':'))}\n\n".encode()


class FeedView:
    """Share the latest encoded browser views without queueing old views."""

    def __init__(self):
        self.changed = Condition()
        self.books = {}
        self.revision = 0
        self.session = 0
        self.state = "connecting"

    def set_state(self, state, books=None):
        with self.changed:
            self.state = state
            self.books = {} if books is None else books
            self.session += 1
            self.revision += 1
            self.changed.notify_all()

    def put_book(self, symbol, frame):
        with self.changed:
            self.books[symbol] = frame
            self.revision += 1
            self.changed.notify_all()

    def wait(self, revision):
        with self.changed:
            self.changed.wait_for(lambda: self.revision != revision, BROWSER_HEARTBEAT_SECONDS)
            return self.revision, self.session, self.state, self.books.copy()


def consume(sock, view):
    with sock.makefile("rb") as stream:
        decoder = Stream(read_message(stream))
        deadline = time.monotonic() + IO_TIMEOUT_SECONDS
        initial = {}
        for _ in decoder.symbols:
            book = decoder.apply(read_message(stream))
            if not book or time.monotonic() > deadline:
                raise FeedError("snapshot did not complete in time")
            initial[book["symbol"]] = event("book", book)
        view.set_state("live", initial)
        while True:
            book = decoder.apply(read_message(stream))
            if book is not None:
                view.put_book(book["symbol"], event("book", book))


def follow(address, view):
    while True:
        try:
            with socket.create_connection(address, IO_TIMEOUT_SECONDS) as sock:
                consume(sock, view)
        except (OSError, ValueError, RecursionError):
            view.set_state("reconnecting")
            time.sleep(RECONNECT_SECONDS)


class BookServer(ThreadingHTTPServer):
    request_queue_size = MAX_HTTP_CLIENTS

    def __init__(self, address, view):
        self.view = view
        self.clients = BoundedSemaphore(MAX_HTTP_CLIENTS)
        super().__init__(address, BookHandler)

    def process_request(self, request, address):
        if not self.clients.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            request.settimeout(IO_TIMEOUT_SECONDS)
            super().process_request(request, address)
        except Exception:
            self.clients.release()
            raise

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            self.clients.release()


class BookHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/events":
            self.stream_book()
            return
        asset = {"/": ("index.html", "text/html"), "/book.js": ("book.js", "text/javascript"),
                 "/book.css": ("book.css", "text/css")}.get(self.path)
        if asset is None:
            self.send_error(404)
            return
        data = (ASSETS / asset[0]).read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", asset[1] + "; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def stream_book(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        view = self.server.view
        revision, session, seen = -1, -1, {}
        try:
            while True:
                revision, next_session, state, books = view.wait(revision)
                if session != next_session:
                    self.wfile.write(event("status", {"state": state}))
                    session, seen = next_session, {}
                for symbol, frame in books.items():
                    if seen.get(symbol) is not frame:
                        self.wfile.write(frame)
                seen = books
                self.wfile.write(b": heartbeat\n\n")
        except OSError:
            return


def port(value):
    number = int(value)
    if not 0 <= number <= 65535:
        raise argparse.ArgumentTypeError("port must be between 0 and 65535")
    return number


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--feed-host", default="127.0.0.1")
    parser.add_argument("--feed-port", type=port, default=9001)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=port, default=8080)
    args = parser.parse_args()
    if args.feed_port == 0:
        parser.error("--feed-port must be greater than zero")
    view = FeedView()
    with BookServer((args.host, args.port), view) as server:
        Thread(target=follow, args=((args.feed_host, args.feed_port), view), daemon=True).start()
        print(f"Order book at http://{args.host}:{server.server_port}", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
