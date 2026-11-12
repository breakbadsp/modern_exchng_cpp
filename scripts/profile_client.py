#!/usr/bin/env python3
"""Run pinned latency measurements and CPU profiles of the in-process client flow.

Builds bench_client and bench_book from the working tree in OUTPUT/build, so the
preset build directory does not change. OUTPUT must not exist; each run writes a new one.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
import time

from perf_control import run_perf

LEVELS = (1, 64, 1024)
REPEATS = 5

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--cpu', type=int, default=min(os.sched_getaffinity(0)))
parser.add_argument('--output', type=Path,
                    help='new directory for the build and results (default: build/client_profile_<UTC time>)')
parser.add_argument('--summarize-only', action='store_true', help='summarize an existing --output again')
parser.add_argument('--lto', choices=('on', 'off'), default='off', help='link-time optimization of the benchmarks')
args = parser.parse_args()


def run(name, command):
    result = (run_perf(command, cwd=root) if command[:2] in (['perf', 'stat'], ['perf', 'record']) else
              subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=root))
    (output / name).write_text(result.stdout)
    if result.returncode:
        raise SystemExit(f'{name} failed: {result.stdout}')
    return result.stdout


def measure():
    output.mkdir(parents=True)
    build = output / 'build'
    run('configure.txt', ['cmake', '-S', str(root), '-B', str(build), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Debug',
                          f'-DCMAKE_INTERPROCEDURAL_OPTIMIZATION={args.lto.upper()}'])
    run('build.txt', ['cmake', '--build', str(build), '-v', '--target', 'bench_client', 'bench_book'])
    compiler = re.search(r'^CMAKE_CXX_COMPILER:\w+=(.+)$', (build / 'CMakeCache.txt').read_text(), re.M)[1]
    metadata = {'utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                'cpu': args.cpu, 'platform': platform.platform(),
                'cpuinfo': Path('/proc/cpuinfo').read_text().split('\n\n')[0],
                'compiler': run('compiler.txt', [compiler, '--version']),
                'head': run('head.txt', ['git', 'rev-parse', 'HEAD']).strip(),
                'status': run('status.txt', ['git', 'status', '--short']),
                'contracts': 'ignore', 'counter_region': 'acknowledged loop window; fixed control/timer overhead included', 'optimization': f'-O3, LTO {args.lto}, frame pointers'}
    (output / 'metadata.json').write_text(json.dumps(metadata, indent=2))
    prefix = ['taskset', '-c', str(args.cpu)]
    client = str(build / 'bench_client')
    run('book.txt', prefix + [str(build / 'bench_book')])
    for levels in LEVELS:
        for repeat in range(REPEATS):
            run(f'batch_{levels}_{repeat}.txt', prefix + [client, 'batch', '1000000', str(levels)])
            run(f'latency_{levels}_{repeat}.txt', prefix + [client, 'latency', '200000', str(levels)])
        command = prefix + [client, 'perf', '20000000', str(levels)]
        run(f'stat_{levels}.txt', ['perf', 'stat', '-e',
            'task-clock,cycles:u,instructions:u,branches:u,branch-misses:u,cache-references:u,cache-misses:u,context-switches,cpu-migrations,page-faults',
            '--'] + command)
        data = output / f'perf_{levels}.data'
        run(f'record_{levels}.txt', ['perf', 'record', '-e', 'cycles:u', '-F', '997',
            '--user-callchains', '--call-graph', 'dwarf,8192', '-o', str(data), '--'] + command)
        for kind, options in [('self', ['--no-children']), ('callers', ['--children'])]:
            run(f'{kind}_{levels}.txt', ['perf', 'report', '-i', str(data), '--stdio',
                '--percent-limit', '0.5', '--sort', 'symbol', '--inline', '--no-call-graph'] + options)
        run(f'lines_{levels}.txt', ['perf', 'report', '-i', str(data), '--stdio',
            '--no-children', '--no-call-graph', '--percent-limit', '0.5', '--sort', 'srcline'])


if args.summarize_only:
    if args.output is None:
        parser.error('--summarize-only needs --output')
    output = args.output.resolve()
else:
    if args.cpu not in os.sched_getaffinity(0):
        parser.error(f'--cpu {args.cpu} is not in this process affinity')
    output = (args.output or root / 'build' / time.strftime('client_profile_%Y%m%dT%H%M%SZ', time.gmtime())).resolve()
    if output.exists():
        parser.error(f'{output} exists; choose a new --output so that earlier recordings stay')
    measure()
summary = {}
for levels in LEVELS:
    batch = [float(re.search(r'wall_ns_per_op,([0-9.]+)', path.read_text())[1])
             for path in sorted(output.glob(f'batch_{levels}_*.txt'))]
    latency = {}
    for path in sorted(output.glob(f'latency_{levels}_*.txt')):
        for line in path.read_text().splitlines()[1:]:
            row = line.split(',')
            latency.setdefault(row[1], []).append(dict(zip(row[2::2], map(int, row[3::2]))))
    summary[levels] = {'best_batch_ns_per_op': min(batch),
                       'median_batch_ns_per_op': statistics.median(batch),
                       'latency': {name: {key: statistics.median(row[key] for row in rows)
                                            for key in rows[0]} for name, rows in latency.items()}}
    print(f'{levels} levels: median batch {summary[levels]["median_batch_ns_per_op"]:.3f} ns/op')
(output / 'summary.json').write_text(json.dumps(summary, indent=2))
print(f'Results: {output}')
