#!/usr/bin/env python3
"""Draw a call graph, grouped by function, from one profile_client.py recording. Requires Graphviz.

Writes callgraph_LEVELS.dot and callgraph_LEVELS.svg next to the recording.

Limits: perf script runs with --no-inline, so inlined code is charged to the function
that contains it. With LTO, GCC inlines most Book code into the client flow, so use a
recording made with --lto off for per-function cost. Frames that match no pattern
below are left out of the edges. A sample whose leaf matches no pattern is 'Other'
(libc, the C++ library, the kernel entry, startup, or an unknown symbol).
"""
import argparse
from collections import Counter
from pathlib import Path
import re
import subprocess

PATTERNS = (
    (re.compile(r'\bmex::Book::(\w+)\('), r'Book::\1'),
    (re.compile(r'\bmex::PriceLadder::(\w+)\('), r'PriceLadder::\1'),
    (re.compile(r'ClientFlow::|\bmex::\(anonymous namespace\)::Run\(|^main$'), 'Client flow'),
    (re.compile(r'\bmex::\(anonymous namespace\)::(\w+)\('), r'\1'),
)
OTHER = 'Other'

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('directory', type=Path, help='a profile_client.py output directory')
parser.add_argument('--levels', type=int, choices=(1, 64, 1024), default=64)
args = parser.parse_args()
recording = args.directory / f'perf_{args.levels}.data'
raw = subprocess.check_output(['perf', 'script', '-i', str(recording),
                               '-F', 'period,ip,sym', '--no-inline'], text=True)


def name(symbol):
    for pattern, label in PATTERNS:
        match = pattern.search(symbol)
        if match:
            return match.expand(label)
    return None


flat, cumulative, edges = Counter(), Counter(), Counter()
total = 0
for sample in raw.strip().split('\n\n'):
    lines = sample.splitlines()
    if not lines:
        continue
    period = int(lines[0].strip())
    total += period
    frames = []
    for line in lines[1:]:
        fields = line.strip().split(maxsplit=1)
        if len(fields) == 2:
            frames.append(name(fields[1]))
    leaf = frames[0] if frames and frames[0] else OTHER
    flat[leaf] += period
    stack = []
    for frame in reversed(frames):
        if frame and (not stack or stack[-1] != frame):
            stack.append(frame)
    if leaf == OTHER:
        stack.append(leaf)
    for frame in set(stack):
        cumulative[frame] += period
    for edge in set(zip(stack, stack[1:])):
        edges[edge] += period
if total == 0:
    raise SystemExit(f'{recording} has no samples')
nodes = sorted(node for node, value in cumulative.items() if value / total >= .005)
identifiers = {node: f'n{index}' for index, node in enumerate(nodes)}
dot = ['digraph profile {', 'graph [ranksep=.65, nodesep=.25];',
       'node [shape=box, style="rounded", fontname="sans-serif", margin=".16,.10"];',
       'edge [fontname="sans-serif", fontsize=10, arrowsize=.7];',
       f'label="{recording}: self = cycles in the function, total = with callees. '
       'Inlined code counts in its caller. Edges below 0.5% hidden.";']
for node in nodes:
    own = flat[node] / total
    color = 'red' if own >= .15 else 'orange' if own >= .05 else 'gray40'
    label = f'{node}\\nself {100 * own:.1f}%\\ntotal {100 * cumulative[node] / total:.1f}%'
    dot.append(f'{identifiers[node]} [label="{label}", color={color}, penwidth={1 + own * 8:.1f}];')
for (parent, child), value in edges.items():
    if parent in identifiers and child in identifiers and value / total >= .005:
        dot.append(f'{identifiers[parent]} -> {identifiers[child]} '
                   f'[label="{100 * value / total:.1f}%", penwidth={1 + 5 * value / total:.2f}];')
dot.append('}')
base = args.directory / f'callgraph_{args.levels}'
base.with_suffix('.dot').write_text('\n'.join(dot) + '\n')
base.with_suffix('.svg').write_text(subprocess.check_output(['dot', '-Tsvg'], input='\n'.join(dot), text=True))
print(base.with_suffix('.svg').resolve())
