#!/usr/bin/env python3
"""Local fault injection game for a cluster of real MEX/libvsr processes."""
import argparse
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time
from urllib.parse import urlsplit

# The cluster size is set here only. Ports, sessions, loops, mission text, and
# the API metadata that the page shows all come from these three values.
# mex_exchange accepts at most 7 replicas.
REPLICA_COUNT = 7
QUORUM = REPLICA_COUNT // 2 + 1
FAULT_LIMIT = REPLICA_COUNT - QUORUM
REPLICAS = range(REPLICA_COUNT)

# The order client reports a stall when no terminal report arrives for this time.
STALL_SECONDS = 3

# Message types and sizes from src/protocol.hpp.
HEADER = struct.Struct('<HB')
NEW = struct.Struct('<QQIBBqI')
LOGON, NEW_ORDER = 1, 2
LOGON_ACK, ACK, REJECT, FILL, DONE = 64, 66, 67, 68, 73
DUPLICATE_REQUEST = 12
REPORT_SIZES = {64: 0, 65: 1, 66: 24, 67: 13, 68: 37, 69: 12,
                70: 13, 71: 24, 72: 13, 73: 13}

ROOT = Path(__file__).resolve().parents[2]
STATIC_FILES = {'/': ('index.html', 'text/html; charset=utf-8'),
                '/game.css': ('game.css', 'text/css; charset=utf-8'),
                '/game.js': ('game.js', 'text/javascript; charset=utf-8')}

# The checks in the order that the player does them.
MISSIONS = [
    dict(id='minority', title=f'Survive {FAULT_LIMIT} failures',
         instruction=f'Crash or freeze {FAULT_LIMIT} replicas. Order reports must continue '
                     f'while only {QUORUM} of {REPLICA_COUNT} replicas run.'),
    dict(id='quorum', title='Remove the quorum',
         instruction=f'Crash or freeze one more replica, so that fewer than {QUORUM} run. '
                     f'Wait until order reports stop for {STALL_SECONDS} seconds.'),
    dict(id='recovery', title='Restore progress',
         instruction=f'Restore replicas until at least {QUORUM} run. '
                     'Wait for the next order report.'),
    dict(id='agreement', title='Check recovered state',
         instruction=f'Select "Verify recovered state". All {REPLICA_COUNT} replicas must '
                     'report the same state digest at the same commit position.'),
]


def exit_reason(code):
    """Describe a Popen return code for a person."""
    return f'signal {signal.Signals(-code).name}' if code < 0 else f'exit code {code}'


def frame(kind, body):
    return HEADER.pack(HEADER.size + len(body), kind) + body


def exact(sock, size):
    data = bytearray()
    while len(data) < size:
        part = sock.recv(size - len(data))
        if not part:
            raise ConnectionError('Gateway disconnected')
        data.extend(part)
    return data


class Client:
    def __init__(self, port, session):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=1.2)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sequence = 0
        try:
            self.sock.sendall(frame(LOGON, session.encode().ljust(16, b'\0') + b'game-secret'.ljust(16, b'\0')))
            if self.receive()[0] != LOGON_ACK:
                raise ConnectionError('Logon rejected')
        except Exception:
            self.close()
            raise

    def receive(self):
        length, kind = HEADER.unpack(exact(self.sock, HEADER.size))
        if kind not in REPORT_SIZES or length != 11 + REPORT_SIZES[kind]:
            raise ValueError('Invalid exchange report')
        body = exact(self.sock, length - HEADER.size)
        sequence = struct.unpack_from('<Q', body)[0]
        if sequence != self.sequence + 1:
            raise ValueError('Report sequence gap')
        self.sequence = sequence
        return kind, body[8:]

    def order(self, number):
        """Send one order. Return (duplicate, fill report count)."""
        # Equal buy/sell quantities keep the resting book bounded across retries.
        self.sock.sendall(frame(NEW_ORDER, NEW.pack(number, number, 1, number % 2, 0, 10000, 1)))
        deadline = time.monotonic() + 2
        fills = 0
        while time.monotonic() < deadline:
            kind, body = self.receive()
            if kind == FILL:
                fills += 1
            elif kind in (ACK, REJECT, DONE) and struct.unpack_from('<Q', body)[0] == number:
                if kind == REJECT and body[-1] != DUPLICATE_REQUEST:
                    raise ValueError(f'Order rejected: reason {body[-1]}')
                if kind == DONE:
                    raise ValueError(f'Unexpected Done: reason {body[-1]}')
                return kind == REJECT, fills
        raise TimeoutError('No terminal order report')

    def close(self):
        self.sock.close()


class Game:
    def __init__(self, binary, rate):
        self.binary = str(binary)
        self.rate = rate
        self.work = Path(tempfile.mkdtemp(prefix='mex-fault-game-'))
        self.lock = threading.RLock()
        self.run_lock = threading.Lock()
        self.stop = threading.Event()
        self.events = deque(maxlen=32)
        self.processes = [None] * REPLICA_COUNT
        self.paused = [False] * REPLICA_COUNT
        self.logs = [None] * REPLICA_COUNT
        # True when the controller stopped the process, or reported its exit.
        self.exit_known = [False] * REPLICA_COUNT
        self.acks = 0
        self.duplicates = 0
        self.fills = 0
        self.number = 1
        self.gateway = None
        self.next_gateway = 1
        self.client = None
        self.last_report = None
        self.error = None
        self.verification = {'status': 'unchecked', 'digests': []}
        self.verifying = False
        self.probe_numbers = [0] * REPLICA_COUNT
        self.completed = set()
        self.lost_quorum = False
        self.samples = deque(maxlen=90)
        self.started = time.monotonic()
        # Each replica needs a replication port and a gateway port. Reserve
        # all ports together. Release just before the replicas bind them.
        reservations = []
        try:
            for _ in range(2 * REPLICA_COUNT):
                sock = socket.socket()
                sock.bind(('127.0.0.1', 0))
                reservations.append(sock)
            ports = [sock.getsockname()[1] for sock in reservations]
            self.ports = ports[REPLICA_COUNT:]
            config = ['engine 0', 'symbol 1 0 4096 64 1 1000000',
                      'session trader game-secret']
            config += [f'session probe{i} game-secret' for i in REPLICAS]
            config += ['gateway address 127.0.0.1', 'gateway port 9000',
                       'gateway max_connections 32', 'gateway inbound_queue_capacity 1024',
                       'gateway outbound_queue_capacity 1024']
            config += [f'replica 127.0.0.1:{ports[i]} {self.ports[i]}' for i in REPLICAS]
            (self.work / 'exchange.cfg').write_text('\n'.join(config) + '\n')
        finally:
            for sock in reservations:
                sock.close()
        try:
            for index in REPLICAS:
                self.start_replica(index)
            # A replica that cannot open its storage or ports exits at once.
            # Report that here, not as seven crashed cards in the page.
            time.sleep(.3)
            for index in REPLICAS:
                code = self.processes[index].poll()
                if code is not None:
                    path = self.work / f'replica{index}.log'
                    log = path.read_text(errors='replace').strip()[-300:] or f'its log {path} is empty'
                    raise RuntimeError(f'Replica {index} stopped during startup with {exit_reason(code)}: {log}')
            self.worker = threading.Thread(target=self.run, name='game-orders')
            self.worker.start()
        except BaseException:
            self.close()
            raise
        self.event(f'Cluster started with {REPLICA_COUNT} replicas.')

    def event(self, message):
        with self.lock:
            self.events.appendleft({'time': round(time.monotonic() - self.started, 1), 'message': message})

    def start_replica(self, index):
        with self.lock:
            if self.stop.is_set():
                raise ValueError('The game is stopping')
            self.logs[index] = open(self.work / f'replica{index}.log', 'ab', buffering=0)
            try:
                self.processes[index] = subprocess.Popen(
                    [self.binary, str(self.work / 'exchange.cfg'), str(index),
                     str(self.work / f'replica{index}.data')], stdout=self.logs[index], stderr=subprocess.STDOUT)
            except Exception:
                self.logs[index].close()
                raise
            self.paused[index] = False
            self.exit_known[index] = False

    def note_exits(self):
        """Add a timeline line for each replica that exited without a request."""
        with self.lock:
            for index in REPLICAS:
                proc = self.processes[index]
                if self.exit_known[index] or self.stop.is_set() or proc.poll() is None:
                    continue
                self.exit_known[index] = True
                self.event(f'Replica {index} exited unexpectedly with {exit_reason(proc.returncode)}. '
                           f'See {self.work / f"replica{index}.log"}.')

    def state(self, index):
        proc = self.processes[index]
        if proc is None or proc.poll() is not None:
            return 'stopped'
        return 'frozen' if self.paused[index] else 'running'

    def running(self):
        return sum(self.state(i) == 'running' for i in REPLICAS)

    def disconnect(self):
        if self.client:
            self.client.close()
        self.client = None
        self.gateway = None

    def run(self):
        next_sample = 0
        while not self.stop.is_set():
            with self.run_lock:
                if self.stop.is_set():
                    break
                with self.lock:
                    verifying = self.verifying
                if not verifying:
                    self.trade()
            self.note_exits()
            now = time.monotonic()
            with self.lock:
                if now >= next_sample:
                    self.samples.append({'time': round(now - self.started, 1), 'reports': self.acks + self.duplicates})
                    next_sample = now + 1
                if (not self.verifying and self.running() < QUORUM
                        and self.last_report is not None and now - self.last_report > STALL_SECONDS):
                    self.completed.add('quorum')
                    self.lost_quorum = True
            self.stop.wait(1 / self.rate)
        self.disconnect()

    def trade(self):
        try:
            if self.client is None:
                candidates = [i for i in REPLICAS if self.state(i) == 'running']
                if not candidates:
                    return
                # Rotate, so a live gateway that stopped serving is not tried again first.
                index = next((i for i in candidates if i >= self.next_gateway), candidates[0])
                self.next_gateway = (index + 1) % REPLICA_COUNT
                self.client = Client(self.ports[index], 'trader')
                with self.lock:
                    self.gateway = index
            duplicate, fills = self.client.order(self.number)
            with self.lock:
                self.number += 1
                self.acks += not duplicate
                self.duplicates += duplicate
                self.fills += fills
                self.last_report = time.monotonic()
                self.error = None
                running = self.running()
                if running == QUORUM:
                    self.completed.add('minority')
                if self.lost_quorum and running >= QUORUM:
                    self.completed.add('recovery')
        except (OSError, ValueError) as error:
            # Keep the exact request number after an uncertain response.
            self.disconnect()
            with self.lock:
                self.error = str(error)
            self.stop.wait(.15)

    def action(self, action, index=None):
        with self.lock:
            if self.stop.is_set():
                raise ValueError('The game is stopping')
            if self.verifying:
                raise ValueError('Wait for state verification to finish')
            if action in ('verify', 'heal') and index is not None:
                raise ValueError(f'{action} takes no replica number')
            if action == 'verify':
                self.verifying = True
                self.error = None
                self.progress('Waiting for the order client to pause.')
                self.verifier = threading.Thread(target=self.verify, name='game-verify')
                self.verifier.start()
                return
            if action == 'heal':
                if any([self.heal(i) for i in REPLICAS]):
                    self.event('All replicas restored. Storage files retained.')
                return
            if action not in ('crash', 'freeze', 'restart') or type(index) is not int or index not in REPLICAS:
                raise ValueError('Invalid fault or replica number')
            proc = self.processes[index]
            state = self.state(index)
            if action == 'restart':
                changed = self.heal(index)
            elif action == 'crash' and state != 'stopped':
                self.exit_known[index] = True
                proc.kill()
                proc.wait(timeout=3)
                self.logs[index].close()
                self.paused[index] = False
                changed = True
            elif action == 'freeze' and state == 'running':
                proc.send_signal(signal.SIGSTOP)
                self.paused[index] = True
                changed = True
            else:
                changed = False
            # A repeated request changes nothing and must not add a timeline line.
            if changed:
                label = {'crash': 'crashed', 'freeze': 'frozen', 'restart': 'restored'}[action]
                self.event(f'Replica {index} {label}. {self.running()} of {REPLICA_COUNT} run.')

    def heal(self, index):
        """Make one replica run again. Return False if it ran already."""
        state = self.state(index)
        if state == 'stopped':
            self.logs[index].close()
            self.start_replica(index)
        elif state == 'frozen':
            self.processes[index].send_signal(signal.SIGCONT)
            self.paused[index] = False
        return state != 'running'

    def progress(self, step, probed=0):
        with self.lock:
            self.verification = {'status': 'checking', 'digests': [], 'step': step, 'probed': probed}

    def verify(self):
        # Quiesce the writer, then send one barrier
        # through each gateway. Digests are available only on graceful shutdown.
        try:
            with self.run_lock:
                self.disconnect()
                with self.lock:
                    if any([self.heal(i) for i in REPLICAS]):
                        self.event('Verification restored all replicas.')
                for index in REPLICAS:
                    self.progress(f'Sending a probe order through the gateway of replica {index}.', index)
                    deadline = time.monotonic() + 20
                    while True:
                        if self.stop.is_set():
                            raise ValueError('Verification stopped')
                        client = None
                        try:
                            client = Client(self.ports[index], f'probe{index}')
                            client.order(self.probe_numbers[index] + 1)
                            self.probe_numbers[index] += 1
                            break
                        except OSError:
                            if time.monotonic() > deadline:
                                raise ValueError(f'Replica {index} did not recover')
                            self.stop.wait(.2)
                        finally:
                            if client:
                                client.close()
                self.progress('Stopping all replicas to read their state digests.', REPLICA_COUNT)
                self.stop.wait(1)
                offsets = [(self.work / f'replica{i}.log').stat().st_size for i in REPLICAS]
                with self.lock:
                    self.exit_known = [True] * REPLICA_COUNT
                for proc in self.processes:
                    proc.send_signal(signal.SIGINT)
                for proc in self.processes:
                    proc.wait(timeout=10)
                digests = []
                for i in REPLICAS:
                    with open(self.work / f'replica{i}.log', 'rb') as log:
                        log.seek(offsets[i])
                        matches = re.findall(rb'state digest ([0-9a-f]+) at operation ([0-9]+)', log.read())
                    if self.processes[i].returncode != 0 or not matches:
                        raise ValueError(f'Replica {i} did not stop cleanly; inspect its log')
                    digest, operation = matches[-1]
                    digests.append({'replica': i, 'digest': digest.decode(), 'operation': int(operation)})
                # Digests are comparable only at one commit position. A replica
                # that stopped before it learned the last commit is not divergent.
                if len({d['operation'] for d in digests}) != 1 or digests[0]['operation'] == 0:
                    status, message = 'inconclusive', 'Replicas stopped at different commit positions. Verify again.'
                elif len({d['digest'] for d in digests}) != 1:
                    status, message = 'failed', 'State digests differ at one commit position. Inspect the retained logs.'
                else:
                    status, message = 'passed', 'State digests and commit positions agree.'
                with self.lock:
                    self.verification = {'status': status, 'digests': digests,
                                         'reports': self.acks + self.duplicates}
                    if status == 'passed':
                        self.completed.add('agreement')
                self.event(message)
        except (OSError, ValueError, subprocess.TimeoutExpired) as error:
            with self.lock:
                self.verification = {'status': 'error', 'digests': [], 'error': str(error)}
            self.event(f'Verification error: {error}')
        finally:
            with self.lock:
                if not self.stop.is_set():
                    for i in REPLICAS:
                        self.heal(i)
                self.verifying = False

    def snapshot(self):
        with self.lock:
            now = time.monotonic()
            age = None if self.last_report is None else round(now - self.last_report, 1)
            return dict(cluster=dict(replica_count=REPLICA_COUNT, quorum=QUORUM, fault_limit=FAULT_LIMIT,
                                     stall_seconds=STALL_SECONDS),
                        replicas=[dict(id=i, pid=p.pid, port=self.ports[i], state=self.state(i))
                                  for i, p in enumerate(self.processes)],
                        running=self.running(),
                        acks=self.acks, duplicates=self.duplicates, fills=self.fills,
                        gateway=self.gateway, report_age=age, error=self.error,
                        verification=self.verification, verifying=self.verifying,
                        missions=[dict(m, done=m['id'] in self.completed) for m in MISSIONS],
                        events=list(self.events), samples=list(self.samples),
                        seconds=round(now - self.started, 1), work_dir=str(self.work))

    def close(self):
        # Every replica start holds self.lock and checks self.stop, so no
        # start can follow these kills. Kill first to release a worker or
        # verifier blocked on socket I/O.
        with self.lock:
            self.stop.set()
            for proc in self.processes:
                if proc is not None and proc.poll() is None:
                    proc.kill()
        for name in ('worker', 'verifier'):
            thread = getattr(self, name, None)
            if thread:
                thread.join(timeout=5)
        for proc in self.processes:
            if proc is not None:
                if proc.poll() is None:
                    proc.kill()
                proc.wait(timeout=5)
        for log in self.logs:
            if log:
                log.close()


class Handler(BaseHTTPRequestHandler):
    def reply(self, status, data, content_type='application/json'):
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = urlsplit(self.path).path
        if path == '/api/state':
            self.reply(200, json.dumps(self.server.game.snapshot()).encode())
        elif path in STATIC_FILES:
            name, content_type = STATIC_FILES[path]
            self.reply(200, Path(__file__).with_name(name).read_bytes(), content_type)
        else:
            self.reply(404, b'{"error":"Not found"}')

    def do_POST(self):
        # Require same-origin JSON. A web page cannot use a simple cross-origin
        # form request to send process signals through this local service.
        origin = self.headers.get('Origin')
        port = self.server.server_port
        allowed = (f'http://127.0.0.1:{port}', f'http://localhost:{port}')
        # Compare only the media type. Clients often add '; charset=utf-8'.
        media_type = self.headers.get('Content-Type', '').split(';')[0].strip().lower()
        if (origin is not None and origin not in allowed) or media_type != 'application/json':
            self.reply(403, b'{"error":"Same-origin JSON required"}')
            return
        try:
            if self.path != '/api/action':
                raise ValueError('Unknown endpoint')
            length = self.headers.get('Content-Length', '0')
            if not length.isdigit() or not 0 < int(length) <= 256:
                raise ValueError('Invalid request length')
            body = json.loads(self.rfile.read(int(length)))
            if not isinstance(body, dict) or not set(body) <= {'action', 'replica'}:
                raise ValueError('Expected an object with only action and replica')
            self.server.game.action(body.get('action'), body.get('replica'))
            self.reply(200, b'{"ok":true}')
        except (ValueError, TypeError) as error:
            self.reply(400, json.dumps({'error': str(error)}).encode())
        except (OSError, subprocess.SubprocessError) as error:
            self.reply(500, json.dumps({'error': f'Process control failed: {error}'}).encode())

    def log_message(self, *_):
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exchange', type=Path, default=ROOT / 'build/mex_exchange')
    parser.add_argument('--port', type=int, default=8080)
    parser.add_argument('--rate', type=int, default=30, help='Orders per second, 1..200')
    args = parser.parse_args()
    if not args.exchange.is_file() or not 0 <= args.port <= 65535 or not 1 <= args.rate <= 200:
        parser.error('Build mex_exchange first. Check --port and --rate (1..200).')
    # Bind before starting processes so an occupied HTTP port cannot leak a cluster.
    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    game = None

    def shutdown(*_):
        raise KeyboardInterrupt
    # A shell starts a background job with SIGINT ignored. Set both handlers,
    # so Ctrl+C and kill -INT or -TERM always stop the game and its replicas.
    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)
    try:
        game = Game(args.exchange.resolve(), args.rate)
        server.game = game
        print(f'MEX fault game: http://127.0.0.1:{server.server_port}', flush=True)
        print(f'Logs and storage: {game.work}', flush=True)
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    except RuntimeError as error:
        raise SystemExit(f'MEX fault game: {error}')
    finally:
        if game:
            game.close()
        server.server_close()


if __name__ == '__main__':
    main()
