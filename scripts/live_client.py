#!/usr/bin/env python3
"""Keep one random-order client connected, with bounded live and pending orders.

Wire layout follows src/protocol.hpp. Reports validate sequence and frame sizes.
Prints counters every five seconds. SIGTERM stops the client. Python allocations
belong to this simulator, not to the exchange.
"""
import argparse
from collections import Counter
import json
import random
import signal
import socket
import struct
import threading
import time

HEADER = struct.Struct('<HB')
NEW = struct.Struct('<QIBBqI')
CANCEL = struct.Struct('<QI')
REPLACE = struct.Struct('<QIqI')
ACK = struct.Struct('<QIQI')
FILL = struct.Struct('<QQIqIIB')
REPLACE_ACK = struct.Struct('<QIqI')
REPORT_SIZES = {64: 0, 65: 1, 66: 24, 67: 13, 68: 37, 69: 12, 70: 13, 71: 24, 72: 13, 73: 13}
MAX_LIVE = 512
MAX_PENDING = 128
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--port', type=int, default=9000)
parser.add_argument('--session', required=True)
parser.add_argument('--token', required=True)
parser.add_argument('--seed', type=int, default=42)
parser.add_argument('--rate', type=int, default=10000)
parser.add_argument('--seconds', type=float, default=0, help='0 runs until stopped')
args = parser.parse_args()
if not 1 <= args.rate <= 100000 or not 1 <= args.port <= 65535 or args.seconds < 0:
    parser.error('Check --rate (1..100000), --port, and --seconds')
if any(not 1 <= len(value.encode('ascii')) <= 16 for value in (args.session, args.token)):
    parser.error('Session and token must be 1..16 ASCII bytes')
stop = threading.Event()
for number in (signal.SIGINT, signal.SIGTERM):
    signal.signal(number, lambda *_: stop.set())
lock = threading.Condition()
live = {}
pending = {}
counts = Counter()
errors = []
rng = random.Random(args.seed)
next_id = time.time_ns()


def frame(kind, body):
    return HEADER.pack(HEADER.size + len(body), kind) + body


def exact(sock, size):
    data = bytearray()
    while len(data) < size:
        try:
            part = sock.recv(size - len(data))
        except socket.timeout:
            if stop.is_set():
                raise EOFError('stopped')
            continue
        if not part:
            raise EOFError('exchange disconnected')
        data.extend(part)
    return data


def receive(sock, expected):
    length, kind = HEADER.unpack(exact(sock, HEADER.size))
    if kind not in REPORT_SIZES or length != 11 + REPORT_SIZES[kind]:
        raise ValueError(f'Invalid report header: {length}, {kind}')
    payload = exact(sock, length - HEADER.size)
    seq = struct.unpack_from('<Q', payload)[0]
    if seq != expected:
        raise ValueError(f'Sequence gap: expected {expected}, got {seq}')
    return kind, payload[8:]


def read_reports(sock):
    expected = 2
    try:
        while not stop.is_set():
            kind, body = receive(sock, expected)
            expected += 1
            with lock:
                counts[f'report_{kind}'] += 1
                if kind == 68:
                    _, order_id, symbol, _, quantity, leaves, _ = FILL.unpack(body)
                    counts['fill_units'] += quantity
                    if leaves == 0:
                        live.pop(order_id, None)
                    elif order_id in live:
                        live[order_id]['quantity'] = leaves
                elif kind >= 66:
                    order_id, symbol = CANCEL.unpack_from(body)
                    request = pending.pop(order_id, None)
                    if request is None:
                        raise ValueError(f'Unexpected terminal report {kind} for {order_id}')
                    counts['completed'] += 1
                    if kind == 66:
                        _, _, _, leaves = ACK.unpack(body)
                        if leaves:
                            live[order_id] = dict(request, quantity=leaves)
                    elif kind == 71:
                        _, _, price, leaves = REPLACE_ACK.unpack(body)
                        if leaves and order_id in live:
                            live[order_id].update(price=price, quantity=leaves)
                        elif leaves == 0:
                            live.pop(order_id, None)
                    elif kind in (69, 73):
                        live.pop(order_id, None)
                    elif kind in (67, 70, 72):
                        reason = body[-1]
                        counts[f'reject_reason_{reason}'] += 1
                        if kind in (70, 72) and reason in (8, 9):
                            live.pop(order_id, None)
                else:
                    raise ValueError(f'Unexpected logon report {kind}')
                lock.notify_all()
    except (OSError, EOFError, ValueError, struct.error) as error:
        if not stop.is_set():
            errors.append(str(error))
            stop.set()
            with lock:
                lock.notify_all()


def next_request():
    global next_id
    candidates = [key for key in live if key not in pending]
    roll = rng.randrange(100)
    if candidates and (len(live) + len(pending) >= MAX_LIVE or roll < 40):
        order_id = rng.choice(candidates)
        order = live[order_id]
        if len(live) + len(pending) >= MAX_LIVE or roll < 25:
            return order_id, 3, CANCEL.pack(order_id, order['symbol']), dict(order)
        price = max(9975, min(10025, order['price'] + rng.randint(-3, 3)))
        return order_id, 4, REPLACE.pack(order_id, order['symbol'], price, rng.randint(1, 100)), dict(order)
    if len(live) + len(pending) >= MAX_LIVE:
        return None
    next_id += 1
    symbol = rng.choice((1, 2))
    side = rng.randrange(2)
    away = rng.randint(-3, 20)
    price = 10000 - away if side == 0 else 10000 + away
    order_type = 1 if rng.randrange(10) == 0 else 0
    quantity = rng.randint(1, 100)
    order = {'symbol': symbol, 'side': side, 'price': price, 'quantity': quantity}
    return next_id, 2, NEW.pack(next_id, symbol, side, order_type, price, quantity), order


with socket.create_connection(('127.0.0.1', args.port), timeout=5) as sock:
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.settimeout(1)
    sock.sendall(frame(1, args.session.encode().ljust(16, b'\0') + args.token.encode().ljust(16, b'\0')))
    kind, _ = receive(sock, 1)
    if kind != 64:
        raise SystemExit('Logon rejected')
    reader = threading.Thread(target=read_reports, args=(sock,))
    reader.start()
    start = time.monotonic()
    deadline = start
    log_at = start + 5
    try:
        while not stop.is_set():
            now = time.monotonic()
            if args.seconds and now - start >= args.seconds:
                break
            # Small paced batches allow several orders in flight without growing history.
            batch = bytearray()
            with lock:
                for _ in range(min(16, MAX_PENDING - len(pending))):
                    request = next_request()
                    if request is None:
                        break
                    order_id, kind, body, order = request
                    pending[order_id] = order
                    counts[f'sent_{kind}'] += 1
                    batch.extend(frame(kind, body))
                if not batch:
                    lock.wait(timeout=.01)
            if batch:
                sock.sendall(batch)
                # Every queued frame has its own length prefix.
                offset = frames = 0
                while offset < len(batch):
                    offset += HEADER.unpack_from(batch, offset)[0]
                    frames += 1
                deadline = max(deadline + frames / args.rate, time.monotonic() - .01)
                stop.wait(max(0, deadline - time.monotonic()))
            if now >= log_at:
                with lock:
                    print(json.dumps({'session': args.session, 'seconds': round(now-start, 1), 'live': len(live), 'pending': len(pending), **counts}), flush=True)
                log_at = now + 5
    finally:
        stop.set()
        sock.shutdown(socket.SHUT_RDWR)
        reader.join(timeout=2)
print(json.dumps({'session': args.session, 'final': True, 'live': len(live), 'pending': len(pending), 'errors': errors, **counts}), flush=True)
if errors:
    raise SystemExit(1)
