#!/usr/bin/env python3
"""Exercise the local game over HTTP with real MEX replicas.

Run: python3 tests/fault_game_test.py [path/to/mex_exchange]
"""
import http.client
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
# The required cluster. The server must report these values and behave to them.
REPLICAS = 7
QUORUM = 4
FAULT_LIMIT = 3


def rendered_page(url):
    """Return the page text after its script ran, or None without Chromium."""
    browser = shutil.which('chromium') or shutil.which('chromium-browser')
    if not browser:
        return None
    with tempfile.TemporaryDirectory() as profile:
        dom = subprocess.run([browser, '--headless', '--disable-gpu', f'--user-data-dir={profile}',
                              '--virtual-time-budget=3000', '--dump-dom', url],
                             capture_output=True, text=True, timeout=60).stdout
    # Remove tags, so the assertions read what a person reads.
    return re.sub(r'\s+', ' ', re.sub(r'<[^>]+>', ' ', dom))


def check_startup_failure():
    """A replica that stops at startup must give its exit status."""
    with tempfile.TemporaryDirectory() as scratch:
        binary = Path(scratch) / 'exits'
        binary.write_text('#!/bin/sh\nexit 3\n')
        binary.chmod(0o755)
        result = subprocess.run([sys.executable, str(ROOT / 'tools/fault_game/server.py'),
                                 '--port', '0', '--exchange', str(binary)],
                                capture_output=True, text=True, timeout=30)
    assert result.returncode != 0, result
    assert re.search(r'Replica 0 stopped during startup with exit code 3: its log \S+replica0.log is empty',
                     result.stderr), result.stderr
    work_dir = re.search(r'(\S+)/replica0.log', result.stderr).group(1)
    if Path(work_dir).name.startswith('mex-fault-game-'):
        shutil.rmtree(work_dir)
    print('PASS: a startup failure gives the exit status', flush=True)


def replica_pids(work_dir):
    """Return the pids of processes that use this game's storage."""
    pids = []
    for cmdline in Path('/proc').glob('[0-9]*/cmdline'):
        try:
            if work_dir.encode() in cmdline.read_bytes():
                pids.append(int(cmdline.parent.name))
        except OSError:
            pass
    return pids


def check_server_kill(binary):
    """A server killed without its shutdown code must not leave replicas."""
    with tempfile.TemporaryFile(mode='w+') as output:
        server = subprocess.Popen([sys.executable, str(ROOT / 'tools/fault_game/server.py'),
                                   '--port', '0', '--exchange', str(binary)],
                                  stdout=output, stderr=subprocess.STDOUT)
        work_dir = None
        try:
            deadline = time.monotonic() + 15
            while work_dir is None and time.monotonic() < deadline and server.poll() is None:
                output.seek(0)
                match = re.search(r'Logs and storage: (\S+)', output.read())
                work_dir = match and match.group(1)
                time.sleep(.1)
            assert work_dir, 'Server did not start'
            assert len(replica_pids(work_dir)) == REPLICAS
            server.kill()
            server.wait()
            deadline = time.monotonic() + 5
            while replica_pids(work_dir) and time.monotonic() < deadline:
                time.sleep(.1)
            remaining = replica_pids(work_dir)
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()
        for pid in remaining:
            os.kill(pid, signal.SIGKILL)
        assert not remaining, f'Replicas outlived a killed server: {remaining}'
        shutil.rmtree(work_dir)
    print('PASS: a killed server leaves no replica processes', flush=True)


def check_delete_storage(binary):
    """With --delete-storage, a clean stop removes the game directory."""
    with tempfile.TemporaryDirectory() as storage, tempfile.TemporaryFile(mode='w+') as output:
        server = subprocess.Popen([sys.executable, str(ROOT / 'tools/fault_game/server.py'), '--port', '0',
                                   '--exchange', str(binary), '--storage-dir', storage, '--delete-storage'],
                                  stdout=output, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline and server.poll() is None:
                output.seek(0)
                if 'Logs and storage' in output.read():
                    break
                time.sleep(.1)
            assert len(list(Path(storage).iterdir())) == 1, 'Game directory not created'
        finally:
            server.send_signal(signal.SIGTERM)
            server.wait(timeout=15)
        assert server.returncode == 0 and not list(Path(storage).iterdir()), list(Path(storage).iterdir())
    print('PASS: --delete-storage removes logs and storage on exit', flush=True)


def main():
    binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / 'build/mex_exchange'
    storage = tempfile.mkdtemp(prefix='mex-fault-test-')
    with tempfile.TemporaryFile(mode='w+') as output:
        # Start as a shell starts a background job: with SIGINT ignored.
        # The server must still stop on SIGINT.
        server = subprocess.Popen([sys.executable, str(ROOT / 'tools/fault_game/server.py'),
                                   '--port', '0', '--rate', '30', '--exchange', str(binary),
                                   '--storage-dir', storage],
                                  stdout=output, stderr=subprocess.STDOUT,
                                  preexec_fn=lambda: signal.signal(signal.SIGINT, signal.SIG_IGN))
        work_dir = None
        try:
            deadline = time.monotonic() + 15
            url = None
            while time.monotonic() < deadline:
                output.seek(0)
                match = re.search(r'http://127.0.0.1:\d+', output.read())
                if match:
                    url = match.group()
                    break
                if server.poll() is not None:
                    raise AssertionError('Game server exited during startup')
                time.sleep(.1)
            assert url, 'Server did not publish its address'

            def state():
                with urllib.request.urlopen(url + '/api/state', timeout=3) as response:
                    return json.load(response)

            def action(kind, replica=None, origin=None):
                headers = {'Content-Type': 'application/json'}
                if origin:
                    headers['Origin'] = origin
                request = urllib.request.Request(url + '/api/action',
                    data=json.dumps(dict(action=kind, replica=replica)).encode(), headers=headers)
                with urllib.request.urlopen(request, timeout=5) as response:
                    assert json.load(response)['ok']

            def until(predicate, description, seconds=25):
                deadline = time.monotonic() + seconds
                latest = None
                while time.monotonic() < deadline:
                    latest = state()
                    if predicate(latest):
                        return latest
                    time.sleep(.15)
                raise AssertionError(f'{description}: {latest}')

            def done(snapshot, mission):
                return next(m['done'] for m in snapshot['missions'] if m['id'] == mission)

            snapshot = state()
            work_dir = snapshot['work_dir']
            # The game directory is in the requested storage directory.
            assert Path(work_dir).parent == Path(storage), (work_dir, storage)
            # The cluster must be seven separate, live exchange processes.
            assert snapshot['cluster']['replica_count'] == REPLICAS, snapshot['cluster']
            assert snapshot['cluster']['quorum'] == QUORUM, snapshot['cluster']
            assert snapshot['cluster']['fault_limit'] == FAULT_LIMIT, snapshot['cluster']
            assert snapshot['running'] == REPLICAS
            pids = {r['pid'] for r in snapshot['replicas']}
            assert len(pids) == REPLICAS and len({r['port'] for r in snapshot['replicas']}) == REPLICAS
            for pid in pids:
                cmdline = Path(f'/proc/{pid}/cmdline').read_bytes()
                assert binary.name.encode() in cmdline and work_dir.encode() in cmdline, cmdline
            print(f'PASS: {REPLICAS} real replica processes run', flush=True)

            # The page and API must be available, and unsafe requests must fail.
            with urllib.request.urlopen(url) as response:
                assert b'FAULT LAB' in response.read()
            for asset, content_type in (('/game.js', 'text/javascript'), ('/game.css', 'text/css')):
                with urllib.request.urlopen(url + asset) as response:
                    assert response.headers['Content-Type'].startswith(content_type) and response.read()
            for payload in ({'action': 'crash', 'replica': -1}, {'action': 'crash', 'replica': True}, [],
                            {'action': 'verify', 'replica': 99}, {'action': 'heal', 'replica': 0},
                            {'action': 'heal', 'extra': 1}):
                request = urllib.request.Request(url + '/api/action', data=json.dumps(payload).encode(),
                                                 headers={'Content-Type': 'application/json'})
                try:
                    urllib.request.urlopen(request)
                    raise AssertionError(f'Invalid action was accepted: {payload}')
                except urllib.error.HTTPError as error:
                    assert error.code == 400
            # A malformed length gets a clean error, not a Python exception text.
            connection = http.client.HTTPConnection(url.removeprefix('http://'), timeout=5)
            connection.putrequest('POST', '/api/action')
            connection.putheader('Content-Type', 'application/json')
            connection.putheader('Content-Length', 'x')
            connection.endheaders()
            response = connection.getresponse()
            assert response.status == 400 and json.load(response) == {'error': 'Invalid request length'}
            connection.close()
            # The media type is case-insensitive and can have parameters.
            for content_type in ('application/json; charset=utf-8', 'Application/JSON'):
                request = urllib.request.Request(url + '/api/action', data=b'{"action":"heal"}',
                                                 headers={'Content-Type': content_type})
                with urllib.request.urlopen(request, timeout=5) as response:
                    assert json.load(response)['ok'], content_type
            request = urllib.request.Request(url + '/api/action', data=b'{"action":"heal"}',
                headers={'Content-Type': 'application/json', 'Origin': 'https://example.com'})
            try:
                urllib.request.urlopen(request)
                raise AssertionError('Cross-origin action was accepted')
            except urllib.error.HTTPError as error:
                assert error.code == 403
            # A page opened as localhost is same-origin too.
            action('heal', origin=url.replace('127.0.0.1', 'localhost'))
            until(lambda s: s['acks'] >= 12 and s['fills'] > 0, 'Initial trading')

            # A person must see the cluster size, the quorum, and each replica.
            page = rendered_page(url)
            if page is None:
                print('SKIP: Chromium not found; rendered page not checked', flush=True)
            else:
                assert f'{REPLICAS} / {REPLICAS} replicas running' in page, page
                assert f'{QUORUM} quorum needed' in page, page
                assert f'{FAULT_LIMIT} more replicas can fail' in page, page
                assert 'Next step Survive 3 failures' in page, page
                assert f'0 / 4 checks completed' in page, page
                for i in range(REPLICAS):
                    assert f'Replica {i} Running' in page, page
                assert 'Server disconnected' not in page and 'No answer from' not in page, page
                print('PASS: page shows seven running replicas, the quorum, and the next step', flush=True)

            # A replica can exit without a player action. The timeline must say
            # so, with the signal, so that it differs from an injected crash.
            os.kill(state()['replicas'][6]['pid'], signal.SIGKILL)
            until(lambda s: any('Replica 6 exited unexpectedly with signal SIGKILL' in e['message']
                                for e in s['events']), 'Unexpected exit report')
            action('restart', 6)
            until(lambda s: s['running'] == REPLICAS, 'Restart after unexpected exit')
            print('PASS: timeline reports an unexpected replica exit', flush=True)

            # A live process can stop serving without the controller knowing.
            # The client must try another gateway with the same pending request.
            snapshot = state()
            gateway_pid = snapshot['replicas'][snapshot['gateway']]['pid']
            before = snapshot['acks']
            os.kill(gateway_pid, signal.SIGSTOP)
            try:
                until(lambda s: s['acks'] > before + 8, 'Gateway retry rotation')
            finally:
                os.kill(gateway_pid, signal.SIGCONT)
            print('PASS: client changes gateway when a live process stops serving', flush=True)

            # Trading must continue with three replicas down. Replica 0 is the
            # initial primary, so this includes a primary failure.
            action('crash', 0)
            action('crash', 0)
            action('crash', 1)
            action('freeze', 2)
            snapshot = state()
            assert snapshot['running'] == QUORUM
            assert [r['state'] for r in snapshot['replicas'][:3]] == ['stopped', 'stopped', 'frozen']
            # A repeated action changes nothing, so the timeline shows it one time.
            assert sum('Replica 0 crashed' in e['message'] for e in snapshot['events']) == 1, snapshot['events']
            before = snapshot['acks']
            until(lambda s: s['acks'] > before + 8 and done(s, 'minority'), 'Progress with three replicas down')
            print(f'PASS: orders continue with {FAULT_LIMIT} of {REPLICAS} replicas down', flush=True)

            # Three running replicas must not keep producing terminal reports.
            action('freeze', 3)
            until(lambda s: done(s, 'quorum'), 'Quorum loss stops reports')
            snapshot = state()
            assert snapshot['running'] == QUORUM - 1
            before = snapshot['acks']
            terminal_reports = snapshot['acks'] + snapshot['duplicates']
            page = rendered_page(url)
            time.sleep(1)
            snapshot = state()
            assert snapshot['acks'] + snapshot['duplicates'] == terminal_reports, 'Reports progressed without a quorum'
            if page is not None:
                assert f'{QUORUM - 1} / {REPLICAS} replicas running' in page, page
                assert 'No quorum. Orders wait.' in page, page
                assert 'Orders cannot commit. Restore 1 replica' in page, page
                assert 'Replica 0 Crashed' in page and 'Replica 3 Frozen' in page, page
                assert 'Next step Restore progress' in page, page
                print('PASS: page explains the lost quorum and the next step', flush=True)
            print(f'PASS: no order reports with {FAULT_LIMIT + 1} of {REPLICAS} replicas down', flush=True)

            action('restart', 0)
            # The request thread that restarted replica 0 has ended. The
            # replica must not die with it.
            restarted = state()['replicas'][0]['pid']
            time.sleep(.5)
            snapshot = state()
            assert snapshot['replicas'][0] == {**snapshot['replicas'][0], 'pid': restarted, 'state': 'running'}
            until(lambda s: s['acks'] > before + 8 and done(s, 'recovery'), 'Recovery resumes trading')
            print('PASS: retained storage recovers and orders resume', flush=True)

            # The game must expose real, equal nonzero state digests and resume.
            for attempt in range(2):
                action('verify')
                result = until(lambda s: not s['verifying'], 'State verification', seconds=45)
                proof = result['verification']
                assert proof['status'] == 'passed', proof
                assert len(proof['digests']) == REPLICAS
                assert len({(d['digest'], d['operation']) for d in proof['digests']}) == 1
                assert proof['digests'][0]['operation'] > 0
                before = result['acks']
                until(lambda s: s['acks'] > before + 5, 'Trading after verification')
                print(f'PASS: verification {attempt + 1}: {proof["digests"]}', flush=True)
            assert all(m['done'] for m in state()['missions']), 'Mission checks did not complete'
            # Crashes and verification stop processes on request. Only the
            # replica killed outside the game is unexpected.
            events = [e['message'] for e in state()['events']]
            assert sum('exited unexpectedly' in m for m in events) == 1, events

            # All replicas can lose power and restart from their own storage.
            for i in range(REPLICAS):
                action('crash', i)
            before = state()['acks']
            action('heal')
            until(lambda s: s['acks'] > before + 8, 'Full cluster restart')
            action('verify')
            result = until(lambda s: not s['verifying'], 'State after full restart', seconds=45)
            assert result['verification']['status'] == 'passed', result['verification']
            print('PASS: all-process crash and recovery preserve state agreement', flush=True)

            # Queue verification behind a writer blocked without a quorum,
            # then stop immediately. The queued verifier must not start replicas.
            for i in range(1, FAULT_LIMIT + 2):
                action('freeze', i)
            until(lambda s: s['gateway'] is not None and s['report_age'] > .25,
                  'Writer blocked before verification')
            action('verify')
        finally:
            server.send_signal(signal.SIGINT)
            timed_out = False
            try:
                server.wait(timeout=15)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
                timed_out = True
            if sys.exc_info()[0] or server.returncode != 0:
                output.seek(0)
                print(output.read(), file=sys.stderr)
            # /proc exists on the Linux platform required by mex_exchange. Find
            # any process that uses this game's storage, including late starts.
            remaining = []
            for cmdline in Path('/proc').glob('[0-9]*/cmdline'):
                try:
                    if work_dir and work_dir.encode() in cmdline.read_bytes():
                        remaining.append(cmdline.parent.name)
                except OSError:
                    pass
            # Clean up even when the assertion fails, so a failed test cannot
            # leave a cluster running on the developer's machine.
            for pid in remaining:
                try:
                    os.kill(int(pid), signal.SIGKILL)
                except ProcessLookupError:
                    pass
            assert not timed_out, 'Server did not clean up'
            assert not remaining, f'Replica processes leaked: {remaining}'
        assert server.returncode == 0, f'Server exit: {server.returncode}'
        # Seven storage files use much space. Keep them only after a failure.
        shutil.rmtree(storage)
    print('PASS: server exit leaves no replica processes')
    check_startup_failure()
    check_server_kill(binary)
    check_delete_storage(binary)


if __name__ == '__main__':
    main()
