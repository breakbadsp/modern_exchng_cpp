"""Check the public argument limits before the tools start a load test."""
from pathlib import Path
import subprocess
import sys


def run(*args):
    return subprocess.run(args, capture_output=True, text=True, timeout=3)


def main():
    loadgen = sys.argv[1]
    driver = Path(__file__).resolve().parents[1] / 'tools/perf_multi_client.sh'
    max_mid = 2**63 - 1 - 22
    for name, value in [('--orders', '16777217'), ('--orders', str(2**64 - 1)),
                        ('--mid', str(max_mid + 1)), ('--mid', '22'), ('--port', '0')]:
        result = run(loadgen, '--host', 'invalid', name, value)
        assert result.returncode == 2 and name in result.stderr.splitlines()[0], result

    # Legal endpoints reach the address check. No exchange or large allocation is needed.
    for mid in (23, max_mid):
        result = run(loadgen, '--mid', str(mid), '--orders', '16777216', '--host', 'invalid')
        assert result.returncode == 1 and 'bad IPv4 address' in result.stderr, result

    for clients in ('0', '9', 'wrong'):
        result = run('bash', driver, '/unused', clients, '1', '1', '0')
        assert result.returncode == 2 and 'clients' in result.stderr, result


if __name__ == '__main__':
    main()
