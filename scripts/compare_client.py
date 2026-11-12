#!/usr/bin/env python3
"""Build Book variants with the same benchmark source, then compare them in interleaved pinned runs.

A variant is NAME SOURCE LTO. NAME is letters, digits, '_' or '-'. SOURCE is 'work'
(the working tree src/), a git commit (src/ at that commit), or a directory of files
that replace files in the working tree src/. LTO is on or off.
Each round runs every variant once per depth, then bench_book once, and rotates the variant order.
The output directory must not exist; each run writes a new one.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import shutil
import statistics
import subprocess
import time

from perf_control import run_perf

LEVELS = (1, 64, 1024)
LATENCY_CYCLES = 200000
MAX_CYCLES = 100000000  # bench_client batch limit
BOOK_WORKLOADS = 6
STAT_EVENTS = 'cycles:u,instructions:u,branches:u,branch-misses:u'

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--cpu', type=int, default=min(os.sched_getaffinity(0)))
parser.add_argument('--output', type=Path,
                    help='new directory for builds and results (default: build/client_compare_<UTC time>)')
parser.add_argument('--base', default='HEAD', help='git commit for the default base variants')
parser.add_argument('--variant', nargs=3, action='append', metavar=('NAME', 'SOURCE', 'LTO'))
parser.add_argument('--rounds', type=int, default=15)
parser.add_argument('--cycles', type=int, default=1000000, help='batch cycles per run; 8 requests per cycle')
parser.add_argument('--latency-rounds', type=int, default=3)
parser.add_argument('--stat-cycles', type=int, default=5000000)
parser.add_argument('--summarize-only', action='store_true', help='summarize an existing --output again')
args = parser.parse_args()


def run(command, **options):
    result = (run_perf(command, cwd=root) if command[:2] == ['perf', 'stat'] else
              subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=root, **options))
    if result.returncode:
        raise SystemExit(f'{" ".join(map(str, command))} failed:\n{result.stdout}')
    return result.stdout


def commit_of(source):
    result = subprocess.run(['git', 'rev-parse', '--verify', '--quiet', f'{source}^{{commit}}'],
                            text=True, stdout=subprocess.PIPE, cwd=root)
    return result.stdout.strip() if result.returncode == 0 else None


def check_arguments():
    for option in ('rounds', 'cycles', 'latency_rounds', 'stat_cycles'):
        value = getattr(args, option)
        if not 0 < value <= MAX_CYCLES:
            parser.error(f'--{option.replace("_", "-")} must be 1..{MAX_CYCLES}')
    if args.stat_cycles < 10000:
        parser.error('--stat-cycles must be at least 10000 to amortize perf control overhead')
    if args.cpu not in os.sched_getaffinity(0):
        parser.error(f'--cpu {args.cpu} is not in this process affinity')
    names = set()
    for name, source, lto in variants:
        if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_-]*', name):
            parser.error(f'variant name {name!r}: use letters, digits, "_" or "-"')
        if name in names:
            parser.error(f'variant name {name!r} is given twice')
        names.add(name)
        if lto not in ('on', 'off'):
            parser.error(f'variant {name}: LTO must be on or off, not {lto!r}')
        if source != 'work' and not Path(source).is_dir() and commit_of(source) is None:
            parser.error(f'variant {name}: {source!r} is not "work", a directory, or a git commit')


def build(name, source, lto):
    tree, build_dir = output / name / 'tree', output / name / 'build'
    tree.mkdir(parents=True)
    # Same CMake rules, tests, and benchmark source for every variant; only src/ differs.
    shutil.copy(root / 'CMakeLists.txt', tree)
    for path in ('tests', 'tools', 'config'):
        shutil.copytree(root / path, tree / path)
    if source == 'work' or Path(source).is_dir():
        shutil.copytree(root / 'src', tree / 'src')
    if source == 'work':
        label = 'working tree'
    elif Path(source).is_dir():
        shutil.copytree(source, tree / 'src', dirs_exist_ok=True)
        label = str(Path(source).resolve())
    else:
        label = commit_of(source)
        archive = subprocess.run(['git', 'archive', label, 'src'], stdout=subprocess.PIPE, check=True, cwd=root)
        subprocess.run(['tar', '-x', '-C', str(tree)], input=archive.stdout, check=True)
    run(['cmake', '-S', str(tree), '-B', str(build_dir), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Debug',
         f'-DCMAKE_INTERPROCEDURAL_OPTIMIZATION={"ON" if lto == "on" else "OFF"}'])
    (output / name / 'build.txt').write_text(run(['cmake', '--build', str(build_dir), '-v',
                                                  '--target', 'bench_client', 'bench_book']))
    compiler = re.search(r'^CMAKE_CXX_COMPILER:\w+=(.+)$', (build_dir / 'CMakeCache.txt').read_text(), re.M)[1]
    return {'source': label, 'lto': lto, 'binary': str(build_dir / 'bench_client'),
            'compiler': run([compiler, '--version']).splitlines()[0]}


def book_rows(text):
    rows = re.findall(r'^(.+?)  (\d+) ops/s  p50 ', text, re.M)
    if len(rows) != BOOK_WORKLOADS:
        raise SystemExit(f'bench_book printed {len(rows)} workloads, expected {BOOK_WORKLOADS}:\n{text}')
    return [(workload, 1e9 / int(ops)) for workload, ops in rows]


def measure():
    output.mkdir(parents=True)
    info = {name: build(name, source, lto) for name, source, lto in variants}
    governor = Path(f'/sys/devices/system/cpu/cpu{args.cpu}/cpufreq/scaling_governor')
    metadata = {'utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'cpu': args.cpu,
                'platform': platform.platform(),
                'governor': governor.read_text().strip() if governor.exists() else 'unknown',
                'head': run(['git', 'rev-parse', 'HEAD']).strip(), 'status': run(['git', 'status', '--short']),
                'rounds': args.rounds, 'cycles': args.cycles, 'latency_rounds': args.latency_rounds,
                'stat_cycles': args.stat_cycles, 'counter_region': 'acknowledged loop window; fixed control/timer overhead included', 'variant_order': [name for name, _, _ in variants],
                'variants': info}
    (output / 'metadata.json').write_text(json.dumps(metadata, indent=2))
    pin = ['taskset', '-c', str(args.cpu)]
    batch, latency, counters, book = [], [], {}, []
    for round_index in range(args.rounds):
        shift = round_index % len(variants)
        order = variants[shift:] + variants[:shift]
        for levels in LEVELS:
            for name, _, _ in order:
                text = run(pin + [info[name]['binary'], 'batch', str(args.cycles), str(levels)])
                value = float(re.search(r'wall_ns_per_op,([0-9.]+)', text)[1])
                batch.append({'round': round_index, 'levels': levels, 'variant': name, 'ns_per_op': value})
        for name, _, _ in order:
            text = run(pin + [str(Path(info[name]['binary']).with_name('bench_book'))])
            for workload, value in book_rows(text):
                book.append({'round': round_index, 'workload': workload, 'variant': name, 'ns_per_op': value})
    for round_index in range(args.latency_rounds):
        shift = round_index % len(variants)
        for levels in LEVELS:
            for name, _, _ in variants[shift:] + variants[:shift]:
                text = run(pin + [info[name]['binary'], 'latency', str(LATENCY_CYCLES), str(levels)])
                for line in text.splitlines()[1:]:
                    row = line.split(',')
                    latency.append({'round': round_index, 'levels': levels, 'variant': name, 'request': row[1],
                                    **dict(zip(row[2::2], map(int, row[3::2])))})
    for name, _, _ in variants:
        for levels in LEVELS:
            text = run(['perf', 'stat', '-x', ',', '-e', STAT_EVENTS, '--'] + pin +
                       [info[name]['binary'], 'perf', str(args.stat_cycles), str(levels)])
            operations = args.stat_cycles * 8
            counters[f'{name}/{levels}'] = {row.split(',')[2].split(':')[0]: int(row.split(',')[0]) / operations
                                            for row in text.splitlines() if row.split(',')[0].isdigit()}
            if set(counters[f'{name}/{levels}']) != {'cycles', 'instructions', 'branches', 'branch-misses'}:
                raise SystemExit('Incomplete perf counters; increase --stat-cycles or check perf access')
    (output / 'raw.json').write_text(json.dumps({'batch': batch, 'latency': latency, 'counters': counters,
                                                'book': book}, indent=2))


variants = args.variant or [['base', args.base, 'off'], ['base_lto', args.base, 'on'],
                            ['work', 'work', 'off'], ['work_lto', 'work', 'on']]
if args.summarize_only:
    if args.output is None:
        parser.error('--summarize-only needs --output')
    output = args.output.resolve()
    names = json.loads((output / 'metadata.json').read_text())['variant_order']
else:
    check_arguments()
    output = (args.output or root / 'build' / time.strftime('client_compare_%Y%m%dT%H%M%SZ', time.gmtime())).resolve()
    if output.exists():
        parser.error(f'{output} exists; choose a new --output so that earlier results stay')
    measure()
    names = [name for name, _, _ in variants]
raw = json.loads((output / 'raw.json').read_text())
summary, lines = {}, ['| Levels | Variant | Median ns/op | Min ns/op | Median ratio to ' + names[0] +
                      ' (min-max) | Cycles/op | Instructions/op | Branch misses/op |', '|' + '---|' * 8]
for levels in LEVELS:
    by_round = {}
    for row in raw['batch']:
        if row['levels'] == levels:
            by_round.setdefault(row['round'], {})[row['variant']] = row['ns_per_op']
    for name in names:
        values = [rounds[name] for rounds in by_round.values()]
        ratios = [rounds[name] / rounds[names[0]] for rounds in by_round.values()]
        stats = raw['counters'][f'{name}/{levels}']
        item = {'median_ns_per_op': statistics.median(values), 'min_ns_per_op': min(values),
                'median_ratio': statistics.median(ratios), 'ratio_min': min(ratios), 'ratio_max': max(ratios),
                'counters_per_op': stats,
                'latency_p50_p99_ns': {request: [statistics.median(r['p50_ns'] for r in rows),
                                                 statistics.median(r['p99_ns'] for r in rows)]
                                       for request in dict.fromkeys(r['request'] for r in raw['latency'])
                                       if (rows := [r for r in raw['latency'] if r['levels'] == levels
                                                    and r['variant'] == name and r['request'] == request])}}
        summary.setdefault(levels, {})[name] = item
        lines.append(f'| {levels} | {name} | {item["median_ns_per_op"]:.3f} | {item["min_ns_per_op"]:.3f} | '
                     f'{item["median_ratio"]:.3f} ({item["ratio_min"]:.3f}-{item["ratio_max"]:.3f}) | '
                     f'{stats["cycles"]:.2f} | {stats["instructions"]:.2f} | {stats["branch-misses"]:.4f} |')
lines += ['', '| bench_book workload | ' + ' | '.join(f'{name} median ns/op' for name in names) + ' |',
          '|' + '---|' * (len(names) + 1)]
for workload in dict.fromkeys(row['workload'] for row in raw['book']):
    medians = {name: statistics.median(row['ns_per_op'] for row in raw['book']
                                       if row['workload'] == workload and row['variant'] == name) for name in names}
    summary.setdefault('bench_book', {})[workload] = medians
    lines.append(f'| {workload} | ' + ' | '.join(f'{medians[name]:.2f}' for name in names) + ' |')
(output / 'summary.json').write_text(json.dumps(summary, indent=2))
(output / 'summary.md').write_text('\n'.join(lines) + '\n')
print('\n'.join(lines))
print(f'Results: {output}')
