# Client profiling report v1.0.0

Measured 2026-11-08 on Intel Core i5-9500, Linux 7.2.15, GCC 16.2.1, CPU 2, `-O3`, contracts ignored, LTO off. Each counter run measured 160 million requests after warm-up. The benchmark uses the in-process book API with a warm book; it does not include network processing or cold caches.

Source HEAD: `1dd36fc3642b35d2081dfbb82658b71ebe51e773`. The benchmark had uncommitted changes; [benchmark.patch](benchmark.patch) records them. Other profiling scripts also had local changes. The book sources were clean. The saved counter output is the authority for this report.

| Price levels | Branch misses | Branch miss rate | Cache misses | Cache miss rate | CPU ns/request |
|---:|---:|---:|---:|---:|---:|
| 1 | 8,998 | 0.00024% | 43,618 | 29.00% | 12.003 |
| 64 | 52,171,604 | 1.12514% | 186,740 | 41.65% | 16.323 |
| 1,024 | 118,634,273 | 2.21722% | 2,162,999 | 35.47% | 20.201 |


Cache miss rate divides `cache-misses` by `cache-references`. These hardware events do not count every memory access. Rates come from one run per depth. CPU time excludes time when the process was not running. Wall time was much higher, so do not use these numbers as latency estimates.

Run `python3 docs/profiling/v1.0.0/generate.py` to regenerate this report and [the diagram](diagram.html) from the saved `stat_*.txt` files. The original profiling command was `python3 scripts/profile_client.py --cpu 2 --lto off --output build/profile_cache_branch_20261108`; exact reproduction also requires the profiling script changes present at measurement time.
