#!/usr/bin/env python3
"""Draw a call graph, grouped by function, from one profile_client.py recording. Requires Graphviz.

Writes callgraph_LEVELS.dot, .svg, .json, and .html next to the recording.

Limits: perf script runs with --no-inline, so inlined code is charged to the function
that contains it. With LTO, GCC inlines most Book code into the client flow, so use a
recording made with --lto off for per-function cost. Frames that match no pattern
below are left out of the edges. A sample whose leaf matches no pattern is 'Other'
(libc, the C++ library, the kernel entry, startup, or an unknown symbol).
"""
import argparse
import html
import json
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
sample_count = 0
for sample in raw.strip().split('\n\n'):
    lines = sample.splitlines()
    if not lines:
        continue
    period = int(lines[0].strip())
    total += period
    sample_count += 1
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
       'label=' + json.dumps(f'CPU call graph: {args.levels} levels per side\nSelf = function; total = function + callees') + ';']
for node in nodes:
    own = flat[node] / total
    color = 'red' if own >= .15 else 'orange' if own >= .05 else 'gray40'
    label = json.dumps(f'{node}\nself {100 * own:.1f}%\ntotal {100 * cumulative[node] / total:.1f}%')
    dot.append(f'{identifiers[node]} [label={label}, color={color}, penwidth={1 + own * 8:.1f}];')
for (parent, child), value in edges.items():
    if parent in identifiers and child in identifiers and value / total >= .005:
        dot.append(f'{identifiers[parent]} -> {identifiers[child]} '
                   f'[label="{100 * value / total:.1f}%", penwidth={1 + 5 * value / total:.2f}];')
dot.append('}')
base = args.directory / f'callgraph_{args.levels}'
base.with_suffix('.dot').write_text('\n'.join(dot) + '\n')
svg = subprocess.check_output(['dot', '-Tsvg'], input='\n'.join(dot), text=True)
base.with_suffix('.svg').write_text(svg)
base.with_suffix('.json').write_text(json.dumps({'total_cycles': total, 'samples': sample_count, 'recording': str(recording), 'nodes': {node: {'self_pct': 100 * flat[node] / total, 'total_pct': 100 * cumulative[node] / total} for node in nodes}}, indent=2))
inline_svg = svg[svg.index('<svg'):]
styles = []
for node in nodes:
    own = flat[node] / total
    color = 'var(--destructive, #b91c1c)' if own >= .15 else 'var(--warning, #b45309)' if own >= .05 else 'var(--chart-2, #2563eb)'
    index = nodes.index(node) + 1
    styles.append(f'#node{index} path,#node{index} polygon{{stroke:{color}}}')
page = """<!doctype html><html><head><meta charset="utf-8"><style>
body{font-family:var(--font-sans, sans-serif);color:var(--foreground, #1f2937)}
svg{width:100%;height:auto}svg>g>polygon{fill:none;stroke:none}
text{fill:var(--foreground, #1f2937)}.node path,.node polygon{fill:var(--card, transparent)}
.edge path{stroke:var(--muted-foreground, #64748b)}
.edge polygon{fill:var(--muted-foreground, #64748b);stroke:var(--muted-foreground, #64748b)}
.node:hover path,.node:hover polygon{fill:var(--secondary, #e2e8f0)}
p,details{font-size:13px;line-height:1.5;color:var(--muted-foreground, #64748b)}details{overflow-wrap:anywhere}
""" + ''.join(styles) + '</style></head><body>'
page += inline_svg
page += f'<p>{sample_count:,} samples, weighted by user CPU cycles. Red: ≥15% self cost; amber: ≥5%. Caller totals overlap. Edges below 0.5% are hidden. Inlined code is charged to its containing function.</p>'
page += '<details><summary>Recording and scope</summary>' + html.escape(str(recording)) + '<p>In-process single-client flow. Read metadata.json for build settings and measurement boundaries.</p></details></body></html>'
base.with_suffix('.html').write_text(page)
print(base.with_suffix('.html').resolve())
