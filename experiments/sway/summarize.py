#!/usr/bin/env python3
"""Markdown tables from run-suite.py's and bench.py's results:
summarize.py RESULTS_DIR"""

import json
import os
import sys

MODES = ("tidewm", "stock", "helper")


def load(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError) as e:
        print(f"<!-- {path}: {e} -->")
        return None


def main(d):
    suites = {m: load(os.path.join(d, f"{m}.json")) for m in MODES}
    print("## Tests passing, per file\n")
    print("| File | Tests | " + " | ".join(MODES) + " |")
    print("|---|---|" + "---|" * len(MODES))
    files = sorted({f for s in suites.values() if s for f in s})
    totals = {m: 0 for m in MODES}
    total = 0
    for f in files:
        n = max(sum((s or {}).get(f, {}).get("counts", {}).values()) for s in suites.values())
        total += n
        cells = []
        for m in MODES:
            c = (suites[m] or {}).get(f, {}).get("counts")
            cells.append(str(c["pass"]) if c else "–")
            totals[m] += c["pass"] if c else 0
        print(f"| {f} | {n} | " + " | ".join(cells) + " |")
    print(f"| **all** | {total} | " + " | ".join(f"**{totals[m]}**" for m in MODES) + " |\n")
    for m in MODES:
        if suites[m]:
            agg = {}
            for f in suites[m].values():
                for k, v in f["counts"].items():
                    agg[k] = agg.get(k, 0) + v
            print(f"- {m}: {agg}")
    print("\n## Benchmarks (ms: median / p95)\n")
    benches = {m: load(os.path.join(d, f"{m}-bench.json")) for m in MODES}
    keys = sorted({k for b in benches.values() if b for k in b.get("summary", {})})
    print("| Measure | " + " | ".join(MODES) + " |")
    print("|---|" + "---|" * len(MODES))
    for k in keys:
        cells = []
        for m in MODES:
            s = ((benches[m] or {}).get("summary") or {}).get(k)
            cells.append(f"{s['median']} / {s['p95']} (n={s['n']})" if s else "–")
        print(f"| {k} | " + " | ".join(cells) + " |")
    print("\n## Frames\n")
    for m in MODES:
        b = benches[m] or {}
        print(f"- {m}: {b.get('frames')}; errors: {b.get('errors')}")
    print("\n## Layout parity with tidewm (x, y, w, h)\n")
    ref = (benches["tidewm"] or {}).get("layouts")
    for m in ("stock", "helper"):
        got = (benches[m] or {}).get("layouts")
        if not ref or not got:
            print(f"- {m}: no data")
            continue
        diffs = [(a["step"], a["rects"], b["rects"]) for a, b in zip(ref, got) if a["rects"] != b["rects"]]
        print(f"- {m}: {len(ref) - len(diffs)}/{len(ref)} steps match exactly")
        for step, a, b in diffs:
            print(f"  - {step}: tidewm {a}, {m} {b}")


if __name__ == "__main__":
    main(sys.argv[1])
