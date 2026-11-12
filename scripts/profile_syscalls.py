#!/usr/bin/env python3
"""Measure thread user/system CPU time and trace syscalls in the warmed client flow.

Build bench_client first. Timing runs are not traced. A separate strace run uses
the two RUSAGE_THREAD reads as boundaries around the measured request loop.
"""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import time

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--binary', type=Path, default=root / 'build/bench_client')
parser.add_argument('--cpu', type=int, default=min(os.sched_getaffinity(0)))
parser.add_argument('--cycles', type=int, default=20000000)
parser.add_argument('--repeats', type=int, default=5)
parser.add_argument('--output', type=Path)
args = parser.parse_args()
if args.cpu not in os.sched_getaffinity(0) or not 1 <= args.cycles <= 100000000 or not 1 <= args.repeats <= 100:
    parser.error('Check --cpu affinity, --cycles (1..100000000), and --repeats (1..100)')
output = (args.output or root / 'build' / time.strftime('syscalls_%Y%m%dT%H%M%SZ', time.gmtime())).resolve()
if output.exists():
    parser.error(f'{output} exists; choose a new --output')
output.mkdir(parents=True)
binary = args.binary.resolve()
prefix = ['taskset', '-c', str(args.cpu), str(binary), 'batch']


def run(command, name):
    result = subprocess.run(command, cwd=root, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
    (output / name).write_text(result.stdout)
    if result.returncode:
        raise SystemExit(result.stdout)
    return result.stdout


runs = []
for repeat in range(args.repeats):
    text = run(prefix + [str(args.cycles), '64'], f'cpu_{repeat}.txt')
    row = next(line.split(',') for line in text.splitlines() if line.startswith('summary,'))
    fields = dict(zip(row[2::2], row[3::2]))
    if 'user_cpu_ns' not in fields:
        raise SystemExit('Rebuild bench_client: this binary does not report user/system CPU time')
    runs.append({key: float(value) for key, value in fields.items()})
trace = output / 'syscalls.trace'
run(['strace', '-qq', '-ttt', '-T', '-o', str(trace), '--'] + prefix + ['1000000', '64'], 'traced_stdout.txt')
lines = trace.read_text().splitlines()
boundaries = [index for index, line in enumerate(lines) if 'getrusage(RUSAGE_THREAD,' in line]
if len(boundaries) != 2:
    raise SystemExit(f'Expected two thread-usage boundaries, found {len(boundaries)}')


def count_calls(rows):
    calls = Counter()
    for row in rows:
        match = re.search(r'\b(\w+)\(', row)
        if match:
            calls[match[1]] += 1
    return dict(calls)


syscalls = {'whole_process': count_calls(lines),
            'measured_window': count_calls(lines[boundaries[0] + 1:boundaries[1]]),
            'traced_requests': 8000000}
users = [row['user_cpu_ns'] for row in runs]
systems = [row['system_cpu_ns'] for row in runs]
operations = args.cycles * 8
summary = {'cpu': args.cpu, 'operations_per_run': operations, 'runs': runs,
           'median_user_cpu_s': statistics.median(users) / 1e9,
           'median_system_cpu_s': statistics.median(systems) / 1e9,
           'aggregate_user_pct': 100 * sum(users) / (sum(users) + sum(systems)),
           'aggregate_system_pct': 100 * sum(systems) / (sum(users) + sum(systems)),
           'syscalls': syscalls,
           'scope': 'In-process warm Book; CPU split excludes setup and warm-up. Thread accounting has microsecond resolution. Syscalls come from a separate traced run.'}
(output / 'summary.json').write_text(json.dumps(summary, indent=2))
(output / 'metadata.json').write_text(json.dumps({'binary': str(binary), 'head': run(['git', 'rev-parse', 'HEAD'], 'head.txt').strip(),
                                                 'utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}, indent=2))
user_pct = summary['aggregate_user_pct']
system_pct = summary['aggregate_system_pct']
page = f'''<!doctype html><html><head><style>
body{{font-family:var(--font-sans)}}p{{font-size:13px;color:var(--muted-foreground)}}
.bar{{display:flex;height:26px;margin:14px 0}}.user{{background:var(--chart-1);width:{user_pct}%}}.system{{background:var(--chart-2);width:{system_pct}%}}
</style></head><body><strong>CPU time · single client, 64 levels</strong><div class="bar"><div class="user"></div><div class="system"></div></div>
<p>User: {user_pct:.4f}% · System: {system_pct:.4f}%</p>
<p>{args.repeats} untraced runs. Setup and warm-up excluded. System calls are checked in a separate trace; tracing time is not used for CPU percentages.</p></body></html>'''
(output / 'cpu_split.html').write_text(page)
print(json.dumps({key: value for key, value in summary.items() if key != 'runs'}, indent=2))
print(output)
