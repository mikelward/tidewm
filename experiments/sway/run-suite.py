#!/usr/bin/env python3
"""Runs tidewm's whole test suite against one compositor and writes what
passed: run-suite.py MODE OUTDIR, MODE being

  tidewm  the dwl binary the Makefile built (the baseline)
  stock   sway alone, through tidewm-on-sway
  helper  sway and tidehelper.py, through tidewm-on-sway

It never fails on test failures: the numbers are the result. OUTDIR gets
MODE.json (per file and per test) and MODE-logs/FILE.txt.
"""

import concurrent.futures
import glob
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TESTS = os.path.join(ROOT, "tests")
LINE = re.compile(r"^(test_\w+) \(([\w.]+)\)(.*)$")


def tests_in(module):
    """Every test's id in the module, without running it"""
    code = ("import unittest,sys\n"
            "def ids(s):\n"
            "    for t in s:\n"
            "        yield from (ids(t) if isinstance(t, unittest.TestSuite) else [t.id()])\n"
            f"print('\\n'.join(ids(unittest.defaultTestLoader.loadTestsFromName('{module}'))))")
    out = subprocess.run([sys.executable, "-c", code], cwd=TESTS, capture_output=True,
                         text=True, timeout=120)
    return [l for l in out.stdout.splitlines() if l]


def run(mode, module, outdir):
    env = dict(os.environ)
    if mode != "tidewm":
        env["TIDEWM"] = os.path.join(HERE, "tidewm-on-sway")
        env["TIDE_SWAY_MODE"] = mode
    ids = tests_in(module)
    status = {}
    log = os.path.join(outdir, f"{mode}-logs", module + ".txt")
    try:
        proc = subprocess.run([sys.executable, "-m", "unittest", "-v", module], cwd=TESTS,
                              env=env, capture_output=True, text=True,
                              timeout=int(os.environ.get("FILE_TIMEOUT", "1500")))
        text = proc.stderr + proc.stdout
    except subprocess.TimeoutExpired as e:
        text = (e.stderr or b"").decode(errors="replace") if isinstance(e.stderr, bytes) else (e.stderr or "")
        text += "\n*** the file timed out\n"
    with open(log, "w") as f:
        f.write(text)
    pending = None
    for line in text.splitlines():
        m = LINE.match(line)
        if m:
            name, where, rest = m.groups()
            pending = where if where.endswith(name) else f"{where}.{name}"
        elif pending:
            rest = line
        else:
            continue
        if "... " not in rest and not rest.endswith("..."):
            continue
        word = rest.rsplit("...", 1)[1].strip()
        for prefix, kind in (("ok", "pass"), ("FAIL", "fail"), ("ERROR", "error"),
                             ("skipped", "skip"), ("expected failure", "pass"),
                             ("unexpected success", "fail")):
            if word.startswith(prefix):
                status[pending] = kind
                pending = None
                break
    for t in ids:
        status.setdefault(t, "timeout")
    return module, status


def main():
    mode, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(os.path.join(outdir, f"{mode}-logs"), exist_ok=True)
    modules = sorted(os.path.basename(p)[:-3] for p in glob.glob(os.path.join(TESTS, "test_*.py")))
    if len(sys.argv) > 3:
        modules = sys.argv[3].split(",")
    results = {}
    with concurrent.futures.ThreadPoolExecutor(int(os.environ.get("JOBS", "4"))) as pool:
        for module, status in pool.map(lambda m: run(mode, m, outdir), modules):
            counts = {k: 0 for k in ("pass", "fail", "error", "skip", "timeout")}
            for s in status.values():
                counts[s] += 1
            results[module] = {"counts": counts, "tests": status}
            print(mode, module, counts, flush=True)
    with open(os.path.join(outdir, f"{mode}.json"), "w") as f:
        json.dump(results, f, indent=1, sort_keys=True)


if __name__ == "__main__":
    main()
