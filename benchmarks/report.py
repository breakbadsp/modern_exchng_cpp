"""Classify completed cases and render a self-contained report."""
from collections import defaultdict
from html import escape
import json
from pathlib import Path

MIN_RATE_FRACTION = 0.98
DRIVER_BUDGET_FRACTION = 0.25


def classify(row, threshold):
    if row.get("status") != "complete":
        return "invalid run"
    if row.get("system_busy", 0):
        return "SystemBusy"
    if (row["ops_s"] < row.get("offered_ops_s", 0) * MIN_RATE_FRACTION
            or row.get("sender_p100_ms", 0) >= threshold * DRIVER_BUDGET_FRACTION):
        return "driver-limited"
    if row["order_p100_ms"] >= threshold:
        return "order P100"
    if row.get("logon_p100_ms", 0) >= threshold:
        return "reconnect P100"
    return "pass"


def limits(rows, threshold):
    groups = defaultdict(lambda: defaultdict(list))
    for row in rows:
        setting = (row.get("base"), row.get("burst"), row.get("window"))
        groups[row["scenario"], setting][row["sessions"]].append(row)
    result = defaultdict(list)
    for (scenario, setting), counts in sorted(groups.items()):
        passed = {count: cases for count, cases in counts.items()
                  if all(classify(case, threshold) == "pass" for case in cases)}
        peak = max((case["ops_s"] for cases in passed.values() for case in cases), default=None)
        result[scenario].append(dict(zip(("base", "burst", "window"), setting)) | {
            "max_tested_passing_sessions": max(passed, default=None),
            "peak_passing_ops_s": peak,
            "sessions_tested": sorted(counts),
        })
    return dict(result)


def rate_limits(rows, threshold):
    groups = defaultdict(lambda: defaultdict(list))
    for row in rows:
        if row["scenario"] in ("active", "hot-symbol"):
            groups[row["scenario"], row["sessions"], row["burst"]][row["base"]].append(row)
    return [
        {"scenario": scenario, "sessions": sessions, "burst": burst,
         "base_rates_tested": sorted(rates),
         "max_tested_passing_base_ops_s": max(
             (rate for rate, cases in rates.items()
              if all(classify(case, threshold) == "pass" for case in cases)), default=None)}
        for (scenario, sessions, burst), rates in sorted(groups.items())
    ]


def chart(rows, key, label, threshold=None):
    maximum = max(([threshold] if threshold else []) + [row.get(key, 0) for row in rows] + [1])
    markers = []
    for i, row in enumerate(rows):
        value = row.get(key)
        if value is None:
            continue
        x = 44 + (i + 0.5) * 550 / len(rows)
        height = 95 * value / maximum
        width = max(2, min(16, 440 / len(rows)))
        status = row["classification"]
        color = "#7fe3bd" if status == "pass" else "#ffba86"
        detail = escape(f"{row['scenario']} · {row['sessions']:,} sessions · {value:,.2f} · {status}")
        markers.append(f'<a href="#run-{i+1}" aria-label="{detail}">'
                       f'<rect x="{x-width/2:.1f}" y="{110-height:.1f}" width="{width:.1f}" '
                       f'height="{height:.1f}" fill="{color}"><title>{detail}</title></rect></a>')
    axis = ""
    if threshold is not None:
        y = 110 - 95 * threshold / maximum
        axis = f'<path d="M44 {y:.1f}H594" stroke="#fd9ca7" stroke-dasharray="5 4"/>'
    description = escape(f"{label}, ordered by the results table. Vertical scale 0 to {maximum:,.2f}.")
    return (f'<figure><figcaption>{escape(label)}</figcaption><svg viewBox="0 0 610 122" '
            f'role="img" aria-label="{description}"><path d="M44 15V110H594" stroke="#8193ab" '
            f'fill="none"/><text x="40" y="19" text-anchor="end">{maximum:,.0f}</text>'
            f'<text x="40" y="114" text-anchor="end">0</text>{"".join(markers)}{axis}</svg></figure>')


def text_cell(value):
    if value is None:
        return "—"
    if isinstance(value, int):
        return f"{value:,}"
    if isinstance(value, float):
        return f"{value:,.2f}"
    return escape(str(value))


def write_report(destination, metadata, cases, threshold):
    rows = [{**row, "classification": classify(row, threshold)} for row in cases]
    summary = limits(rows, threshold)
    rates = rate_limits(rows, threshold)
    (destination / "results.json").write_text(
        json.dumps({"environment": metadata, "target_ms": threshold,
                    "limits": summary, "rate_limits": rates, "runs": rows}, indent=2) + "\n",
        encoding="utf-8",
    )

    columns = (
        ("Scenario", "scenario"), ("Sessions", "sessions"), ("Run", "repeat"),
        ("Base", "base"), ("Burst +", "burst"), ("Window", "window"),
        ("Ops/s", "ops_s"), ("Order P100 ms", "order_p100_ms"),
        ("Steady RTT ms", "steady_rtt_p100_ms"), ("Burst RTT ms", "burst_rtt_p100_ms"),
        ("Send lag ms", "sender_p100_ms"), ("Logon P100 ms", "logon_p100_ms"),
        ("Result", "classification"),
    )
    header = "".join(f'<th scope="col">{label}</th>' for label, _ in columns)
    table = "".join(
        f'<tr id="run-{index}">'
        + "".join(f"<td>{text_cell(row.get(key))}</td>" for _, key in columns) + "</tr>"
        for index, row in enumerate(rows, start=1)
    )
    limit_rows = "".join(
        f"<tr><td>{escape(scenario)}</td><td>{text_cell(item['base'])}</td>"
        f"<td>{text_cell(item['burst'])}</td><td>{text_cell(item['window'])}</td>"
        f"<td>{text_cell(item['max_tested_passing_sessions'])}</td>"
        f"<td>{text_cell(item['peak_passing_ops_s'])}</td></tr>"
        for scenario, items in summary.items() for item in items
    )
    rate_rows = "".join(
        f"<tr><td>{escape(row['scenario'])}</td><td>{row['sessions']:,}</td>"
        f"<td>{row['burst']:,}</td><td>{text_cell(row['max_tested_passing_base_ops_s'])}</td></tr>"
        for row in rates
    )
    rate_table = (
        '<h2>Largest tested passing base rates</h2><div class="scroll"><table><thead><tr>'
        '<th>Scenario</th><th>Sessions</th><th>Burst +</th><th>Base ops/s</th>'
        f'</tr></thead><tbody>{rate_rows}</tbody></table></div>'
    ) if rates else ""
    errors = "".join(
        f"<li>{escape(row['scenario'])}, {row['sessions']:,} sessions, run {row['repeat']}. "
        f"{escape(row['error'])}</li>" for row in rows if row.get("error")
    )
    failures = f"<details><summary>Invalid runs</summary><ul>{errors}</ul></details>" if errors else ""
    environment = escape(json.dumps(metadata, indent=2))
    html = '''<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>MEX performance</title>
<style>
*{box-sizing:border-box}body{margin:0;font:14px/1.5 system-ui,sans-serif}
main{max-width:1000px;margin:auto}section{padding:16px;border-radius:12px;background:#101b2d;color:#edf5ff}
h1,h2,p{margin:0}h1{font-size:24px}h2{font-size:16px;margin:12px 0 4px}
p{color:#bbcbe0;margin:5px 0 10px}figure{margin:10px 0}
svg{width:100%;height:126px;display:block}svg text{fill:#c3cfe1;font-size:10px}
table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}
td,th{text-align:left;padding:5px 7px;border-top:1px solid #3c4b62;white-space:nowrap}
.scroll{overflow:auto}pre{overflow:auto}summary{cursor:pointer;color:#a7cfff}
tr:target{background:#30476a}svg a:focus rect{stroke:white;stroke-width:2}
</style></head><body><main><section>'''
    html += (f"<h1>MEX performance</h1><p>{len(rows)} runs · P100 target below {threshold:g} ms. "
             "Select a bar to see its run. Green marks a passing run.</p>")
    html += chart(rows, "ops_s", "Completed operations per second")
    html += chart(rows, "order_p100_ms", "Order P100 in milliseconds", threshold)
    if any(row.get("logon_p100_ms") is not None for row in rows):
        html += chart(rows, "logon_p100_ms", "Reconnect P100 in milliseconds", threshold)
    html += (
        "<h2>Largest tested passing settings</h2><p>All repeats at a setting must pass. "
        "A measured limit applies to the workload, I/O backend, and machine shown here.</p>"
        '<div class="scroll"><table><thead><tr><th>Scenario</th><th>Base</th><th>Burst +</th>'
        '<th>Window</th><th>Max sessions</th><th>Peak passing ops/s</th></tr></thead>'
        f"<tbody>{limit_rows}</tbody></table></div>{rate_table}"
        '<h2>Run results</h2><div class="scroll"><table aria-label="Run results"><thead><tr>'
        f"{header}</tr></thead><tbody>{table}</tbody></table></div>{failures}"
        f"<details><summary>Environment</summary><pre>{environment}</pre></details>"
        "<p>See results.json for all measurements. Active and hot-symbol P100 starts at the scheduled send. "
        "Saturation and reconnect order P100 starts at entry to the client send batch. "
        "Reconnect P100 measures TCP connect through LogonAck.</p></section></main></body></html>"
    )
    path = destination / "report.html"
    path.write_text(html, encoding="utf-8")
    return path
