#!/usr/bin/env python3
"""Generate the report and chart from the saved perf stat output."""
from pathlib import Path
import re

root = Path(__file__).resolve().parent
rows = []
for depth in (1, 64, 1024):
    data = (root / f'stat_{depth}.txt').read_text()
    def event(name):
        return int(re.search(r'^\s*([\d,]+)\s+' + re.escape(name) + r':u', data, re.M).group(1).replace(',', ''))
    cpu = float(re.search(r'cpu_ns_per_op,([\d.]+)', data).group(1))
    branches, misses = event('branches'), event('branch-misses')
    refs, cache_misses = event('cache-references'), event('cache-misses')
    rows.append((depth, misses, 100 * misses / branches, cache_misses, 100 * cache_misses / refs, cpu))

header = '''# Client profiling report v1.0.0

Measured 2026-11-08 on Intel Core i5-9500, Linux 7.2.15, GCC 16.2.1, CPU 2, `-O3`, contracts ignored, LTO off. Each counter run measured 160 million requests after warm-up. The benchmark uses the in-process book API with a warm book; it does not include network processing or cold caches.

Source HEAD: `1dd36fc3642b35d2081dfbb82658b71ebe51e773`. The benchmark had uncommitted changes; [benchmark.patch](benchmark.patch) records them. Other profiling scripts also had local changes. The book sources were clean. The saved counter output is the authority for this report.

| Price levels | Branch misses | Branch miss rate | Cache misses | Cache miss rate | CPU ns/request |
|---:|---:|---:|---:|---:|---:|
'''
lines = [f'| {d:,} | {b:,} | {br:.5f}% | {c:,} | {cr:.2f}% | {cpu:.3f} |' for d,b,br,c,cr,cpu in rows]
tail = '''

Cache miss rate divides `cache-misses` by `cache-references`. These hardware events do not count every memory access. Rates come from one run per depth. CPU time excludes time when the process was not running. Wall time was much higher, so do not use these numbers as latency estimates.

Run `python3 docs/profiling/v1.0.0/generate.py` to regenerate this report and [the diagram](diagram.html) from the saved `stat_*.txt` files. The original profiling command was `python3 scripts/profile_client.py --cpu 2 --lto off --output build/profile_cache_branch_20261108`; exact reproduction also requires the profiling script changes present at measurement time.
'''
(root / 'README.md').write_text(header + '\n'.join(lines) + '\n' + tail)
html = '''<!doctype html><html><head><meta charset="utf-8"><style>body{font:14px var(--font-sans, sans-serif);color:var(--foreground);margin:8px}h2{font-size:16px;margin:18px 0 8px}.row{display:flex;align-items:center;gap:8px;margin:8px 0}.label{width:70px}.track{flex:1;background:var(--secondary);border-radius:3px}.bar{height:20px;background:var(--chart-1);border-radius:3px}.value{width:90px;text-align:right}small{color:var(--muted-foreground)}</style></head><body><small>Client profiling v1.0.0 · 160 million requests per depth</small>'''
for title, index, unit in [('Branch misses per request', 1, ''), ('Cache misses per request', 3, ''), ('CPU time per request', 5, ' ns')]:
    values = [row[index] / 160_000_000 if index in (1, 3) else row[index] for row in rows]
    html += f'<h2>{title}</h2>'
    for row, value in zip(rows, values):
        html += f'<div class="row"><span class="label">{row[0]:,} levels</span><div class="track"><div class="bar" style="width:{100*value/max(values):.2f}%"></div></div><span class="value">{value:.5f}{unit}</span></div>'
html += '</body></html>\n'
(root / 'diagram.html').write_text(html)
