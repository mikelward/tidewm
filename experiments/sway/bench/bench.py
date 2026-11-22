#!/usr/bin/env python3
"""Measures one compositor the same way for tidewm and for sway + the helper:

    bench.py MODE OUTDIR [RUNS]

MODE is tidewm, helper or stock, as for run-suite.py. It writes
OUTDIR/MODE-bench.json: raw samples and summaries, and OUTDIR/frames/MODE-*.png.

Two views of each transition:
- what IPC clients see: the reply, or the event, through the socket tide
  uses (the helper's proxy, or tidewm's);
- what the compositor holds: a thread polls GET_TREE on the compositor's
  own socket (sway's real one, past the helper) as fast as it answers,
  recording every window's rect and focus with a timestamp. Intermediate
  states, as a window in sway's own place before the helper moves it, show
  up here. sway's tree is its pending state; a frame shows it once the
  clients have drawn (its transactions), so frames (grim) are sampled too.

Every time is time.monotonic(), the clock the helper stamps its log with.
"""

import glob
import json
import os
import shutil
import statistics
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "tests"))

MODE = sys.argv[1]
if MODE != "tidewm":
    os.environ["TIDEWM"] = os.path.join(os.path.dirname(HERE), "tidewm-on-sway")
    os.environ["TIDE_SWAY_MODE"] = MODE

import harness  # noqa: E402
from harness import (EVENT_WINDOW, GET_TREE, GET_VERSION, RUN_COMMAND, Ipc,  # noqa: E402
                     Tidewm)


def windows(tree):
    found = {}

    def walk(node):
        if node.get("type") in ("con", "floating_con") and (node.get("app_id") is not None
                                                              or node.get("window")):
            r = node["rect"]
            found[node["id"]] = ((r["x"], r["y"], r["width"], r["height"]), node["focused"])
        for c in node.get("nodes", []) + node.get("floating_nodes", []):
            walk(c)
    walk(tree)
    return found


class Poller:
    """Polls the compositor's own tree in a thread"""

    def __init__(self, path):
        self.ipc = Ipc(path)
        self.samples = []
        self.running = False

    def start(self):
        self.samples = []
        self.running = True
        self.thread = threading.Thread(target=self.loop)
        self.thread.start()

    def loop(self):
        while self.running:
            tree = self.ipc.request(GET_TREE)
            self.samples.append((time.monotonic(), windows(tree)))

    def stop(self):
        self.running = False
        self.thread.join()
        return self.samples


class Framer:
    """grim in a loop in a thread: timestamped frames"""

    def __init__(self, env):
        self.env = env
        self.frames = []
        self.ok = shutil.which("grim") is not None

    def start(self):
        self.frames = []
        self.running = self.ok
        if self.ok:
            self.thread = threading.Thread(target=self.loop)
            self.thread.start()

    def loop(self):
        while self.running:
            t = time.monotonic()
            p = subprocess.run(["grim", "-t", "png", "-l", "0", "-"], env=self.env,
                               capture_output=True, timeout=10)
            if p.returncode == 0:
                self.frames.append((t, p.stdout))
            else:
                self.ok = self.running = False

    def stop(self):
        if self.ok:
            self.running = False
            self.thread.join()
        return self.frames


def raw_socket(wm):
    if MODE == "tidewm" or MODE == "stock":
        return wm.socket
    socks = glob.glob(os.path.join(wm.dir.name, "sway-ipc.*.sock"))
    return socks[0]


def helper_stamps(wm, since):
    path = os.path.join(wm.dir.name, "tide-sway", "helper-times.jsonl")
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return [s for s in map(json.loads, f) if s["t"] >= since]


def settle(samples, after):
    """From samples after `after`: the final state, when it was first
    reached for good, and the distinct states before it"""
    samples = [s for s in samples if s[0] >= after]
    if not samples:
        return None, None, []
    final = samples[-1][1]
    t_final = samples[-1][0]
    for t, state in reversed(samples):
        if state != final:
            break
        t_final = t
    states = []
    for t, state in samples:
        if t >= t_final:
            break
        if not states or states[-1][1] != state:
            states.append((t, state))
    return final, t_final, states


def ms(a, b):
    return None if a is None or b is None else round((b - a) * 1000, 3)


def summary(values):
    values = sorted(v for v in values if v is not None)
    if not values:
        return None
    p95 = values[min(len(values) - 1, int(round(0.95 * (len(values) - 1))))]
    return {"n": len(values), "median": round(statistics.median(values), 3), "p95": p95,
            "max": values[-1]}


def drain_window_event(ipc, change, con_id=None):
    return ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == change and (
        con_id is None or e["container"]["id"] == con_id))


def quiet(seconds):
    """The bench, not a test, may wait: lets late changes land in the samples"""
    time.sleep(seconds)


def bench_open_close(wm, runs, out):
    """A third window opening on a master and one stack window, then closing"""
    ipc = wm.ipc()
    ipc.subscribe("window")
    base = []
    for name in ("bench.a", "bench.b"):
        base.append(wm.open_window(name, name))
        drain_window_event(ipc, "new")
    poller = Poller(raw_socket(wm))
    rows = []
    for i in range(runs):
        poller.start()
        quiet(0.02)
        t0 = time.monotonic()
        client = wm.open_window("bench.new", "bench.new")
        event = drain_window_event(ipc, "new")
        t_event = time.monotonic()
        cid = event["container"]["id"]
        quiet(0.25)
        samples = poller.stop()
        seen = next((t for t, s in samples if cid in s), None)
        final, t_final, states = settle(samples, t0)
        stamps = [s for s in helper_stamps(wm, t0)]
        recv = next((s["t"] for s in stamps if s["ev"] == "new_recv" and s["con"] == cid), None)
        sent = next((s["t"] for s in stamps if s["ev"] == "layout_sent" and recv and s["t"] >= recv), None)
        done = next((s["t"] for s in stamps if s["ev"] == "layout_done" and sent and s["t"] >= sent), None)
        wrong = [st for st in states if cid in st[1]]
        focus_flips = sum(1 for a, b in zip(states, states[1:])
                          if {k for k, v in a[1].items() if v[1]} != {k for k, v in b[1].items() if v[1]})
        rows.append({"kind": "open", "spawn_to_seen": ms(t0, seen), "seen_to_event": ms(seen, t_event),
                     "seen_to_helper_recv": ms(seen, recv), "seen_to_layout_sent": ms(seen, sent),
                     "seen_to_layout_done": ms(seen, done), "seen_to_settled": ms(seen, t_final),
                     "intermediate_states": len(wrong), "focus_changes_before_settled": focus_flips,
                     "samples": len(samples)})
        # close it
        poller.start()
        quiet(0.02)
        t0 = time.monotonic()
        client.kill()
        client.wait()
        drain_window_event(ipc, "close", cid)
        t_event = time.monotonic()
        quiet(0.25)
        samples = poller.stop()
        gone = next((t for t, s in samples if t >= t0 and cid not in s), None)
        final, t_final, states = settle(samples, t0)
        wrong = [st for st in states if cid not in st[1]]
        rows.append({"kind": "close", "kill_to_gone": ms(t0, gone), "gone_to_event": ms(gone, t_event),
                     "gone_to_settled": ms(gone, t_final), "intermediate_states": len(wrong),
                     "samples": len(samples)})
        wm.clients.remove(client)
    out["open_close"] = rows
    ipc.close()


def bench_commands(wm, runs, out):
    """Workspace switches and focus changes: the reply's round trip, and
    when the compositor's tree settles"""
    ipc = wm.ipc()
    ipc.subscribe("window")
    for name in ("bench.c", "bench.d", "bench.e"):
        wm.open_window(name, name)
        drain_window_event(ipc, "new")
    poller = Poller(raw_socket(wm))
    cmd = wm.ipc()
    rows = []
    for i in range(runs):
        for kind, command in (("workspace", "workspace two"), ("workspace", "workspace 1"),
                              ("focus", "focus next")):
            poller.start()
            quiet(0.01)
            t0 = time.monotonic()
            reply = cmd.request(RUN_COMMAND, command)
            t_reply = time.monotonic()
            quiet(0.1)
            samples = poller.stop()
            final, t_final, states = settle(samples, t0)
            rows.append({"kind": kind, "reply": ms(t0, t_reply), "settled": ms(t0, t_final),
                         "intermediate_states": max(len(states) - 1, 0),
                         "ok": all(r.get("success") for r in reply)})
    out["commands"] = rows
    cmd.close()
    ipc.close()


def bench_guard(wm, runs, out):
    """With the focus guard: a granted window's time to focus, and whether
    a held window ever holds focus, even briefly"""
    ipc = wm.ipc()
    ipc.subscribe("window")
    cmd = wm.ipc()
    cmd.request(RUN_COMMAND, "grant bench.first")
    wm.open_window("bench.first", "bench.first")
    first = drain_window_event(ipc, "new")["container"]["id"]
    poller = Poller(raw_socket(wm))
    rows = []
    for i in range(runs):
        poller.start()
        quiet(0.01)
        t0 = time.monotonic()
        cmd.request(RUN_COMMAND, "grant bench.granted")
        c = wm.open_window("bench.granted", "bench.granted")
        cid = drain_window_event(ipc, "new")["container"]["id"]
        t_event = time.monotonic()
        quiet(0.2)
        samples = poller.stop()
        seen = next((t for t, s in samples if cid in s), None)
        focused = next((t for t, s in samples if cid in s and s[cid][1]), None)
        rows.append({"kind": "granted", "seen_to_focused": ms(seen, focused),
                     "seen_to_event": ms(seen, t_event)})
        # a held one: does it ever have focus?
        poller.start()
        quiet(0.01)
        h = wm.open_window("bench.held", "bench.held")
        hid = drain_window_event(ipc, "new")["container"]["id"]
        quiet(0.2)
        samples = poller.stop()
        held_focus = [t for t, s in samples if hid in s and s[hid][1]]
        rows.append({"kind": "held", "samples_with_wrong_focus": len(held_focus),
                     "wrong_focus_ms": ms(held_focus[0], held_focus[-1]) if held_focus else 0,
                     "samples": len(samples)})
        for client, con in ((c, cid), (h, hid)):
            client.kill()
            client.wait()
            drain_window_event(ipc, "close", con)
            wm.clients.remove(client)
        cmd.request(RUN_COMMAND, f"[con_id={first}] focus")
    out["guard"] = rows
    cmd.close()
    ipc.close()


def bench_ipc(wm, runs, out):
    """Round trips: through the socket tide uses, and to the compositor's own"""
    ipc = wm.ipc()
    ipc.subscribe("window")
    for i in range(20 - len(wm.clients)):
        wm.open_window(f"bench.t{i}", f"bench.t{i}")
        drain_window_event(ipc, "new")
    rows = {}
    for label, path in (("proxy", wm.socket), ("direct", raw_socket(wm))):
        conn = Ipc(path)
        for kind, name in ((GET_VERSION, "get_version"), (GET_TREE, "get_tree")):
            times = []
            for _ in range(runs * 4):
                t0 = time.monotonic()
                conn.request(kind)
                times.append((time.monotonic() - t0) * 1000)
            rows[f"{name}_{label}"] = times
        conn.close()
    out["ipc"] = rows
    ipc.close()


def bench_layouts(wm, out):
    """tile with 1 to 5 windows, then a close and a swap of the master"""
    ipc = wm.ipc()
    ipc.subscribe("window")
    cmd = wm.ipc()
    cmd.request(RUN_COMMAND, "layout tile")
    cons, clients, steps = [], [], []

    def record(step):
        found = windows(cmd.request(GET_TREE))
        steps.append({"step": step, "rects": [found[c][0] if c in found else None for c in cons]})

    for i in range(5):
        clients.append(wm.open_window(f"bench.l{i}", f"bench.l{i}"))
        cons.append(drain_window_event(ipc, "new")["container"]["id"])
        record(f"open {i + 1}")
    clients[1].kill()
    clients[1].wait()
    drain_window_event(ipc, "close", cons[1])
    wm.clients.remove(clients[1])
    cons.pop(1)
    record("close the first stack window")
    cmd.request(RUN_COMMAND, f"[con_id={cons[2]}] focus")
    cmd.request(RUN_COMMAND, "zoom")
    record("zoom the third")
    out["layouts"] = steps
    cmd.close()
    ipc.close()


def bench_frames(wm, outdir, out):
    """Frames across a third window opening, and the look of three windows"""
    framer = Framer(wm.env)
    if not framer.ok:
        out["frames"] = "no grim"
        return
    ipc = wm.ipc()
    ipc.subscribe("window")
    for name in ("bench.f0", "bench.f1"):
        wm.open_window(name, name)
        drain_window_event(ipc, "new")
    quiet(0.2)
    framer.start()
    quiet(0.2)
    t0 = time.monotonic()
    wm.open_window("bench.f2", "bench.f2")
    drain_window_event(ipc, "new")
    quiet(0.5)
    frames = framer.stop()
    os.makedirs(os.path.join(outdir, "frames", MODE), exist_ok=True)
    distinct, last = [], None
    for t, png in frames:
        if png != last:
            distinct.append((t, png))
            last = png
    for i, (t, png) in enumerate(distinct):
        with open(os.path.join(outdir, "frames", MODE, f"open-{i:02d}-{ms(t0, t):+.0f}ms.png"), "wb") as f:
            f.write(png)
    with open(os.path.join(outdir, "frames", f"{MODE}-three-windows.png"), "wb") as f:
        f.write(frames[-1][1])
    gaps = [ms(a[0], b[0]) for a, b in zip(frames, frames[1:])]
    out["frames"] = {"captured": len(frames), "distinct": len(distinct),
                     "intermediate": max(len(distinct) - 2, 0),
                     "frame_interval_ms": summary(gaps)}
    ipc.close()


def bench_flicker(wm, outdir, cycles, out):
    """A third window opening and closing, cycles times, with grim taking
    every frame it can: how many cycles showed a frame that is neither the
    layout before nor the layout after, and for how long"""
    framer = Framer(wm.env)
    if not framer.ok:
        out["flicker"] = "no grim"
        return
    ipc = wm.ipc()
    ipc.subscribe("window")
    for name in ("bench.f0", "bench.f1"):
        wm.open_window(name, name)
        drain_window_event(ipc, "new")
    quiet(0.3)
    rows = []
    saved = {"open": False, "close": False}
    os.makedirs(os.path.join(outdir, "frames", MODE), exist_ok=True)
    for i in range(cycles):
        for kind in ("open", "close"):
            framer.start()
            quiet(0.1)
            t0 = time.monotonic()
            if kind == "open":
                client = wm.open_window("bench.f2", "bench.f2")
                cid = drain_window_event(ipc, "new")["container"]["id"]
            else:
                client.kill()
                client.wait()
                wm.clients.remove(client)
                drain_window_event(ipc, "close", cid)
            quiet(0.3)
            frames = framer.stop()
            if len(frames) < 3:
                continue
            before, after = frames[0][1], frames[-1][1]
            wrong = [(t, png) for t, png in frames if png != before and png != after]
            rows.append({"kind": kind, "frames": len(frames), "wrong_frames": len(wrong),
                         "wrong_ms": ms(wrong[0][0], wrong[-1][0]) + 16.7 if wrong else 0,
                         "first_wrong_after_ms": ms(t0, wrong[0][0]) if wrong else None})
            if wrong and not saved[kind]:
                saved[kind] = True
                strip = [frames[0]] + wrong + [frames[-1]]
                for j, (t, png) in enumerate(strip):
                    name = f"flicker-{kind}-{j:02d}-{ms(t0, t):+.0f}ms.png"
                    with open(os.path.join(outdir, "frames", MODE, name), "wb") as f:
                        f.write(png)
    out["flicker"] = rows
    ipc.close()


def run(name, outdir, out, fn, config=None):
    wm = Tidewm(1, config)
    try:
        fn(wm)
    except Exception as e:
        out.setdefault("errors", {})[name] = repr(e)
        print(f"{MODE} {name}: {e!r}\n{wm.output()[-3000:]}", file=sys.stderr)
    finally:
        wm.stop()


def main():
    outdir = sys.argv[2]
    runs = int(sys.argv[3]) if len(sys.argv) > 3 else 50
    os.makedirs(outdir, exist_ok=True)
    out = {"mode": MODE, "runs": runs}
    run("open_close", outdir, out, lambda wm: bench_open_close(wm, runs, out))
    run("commands", outdir, out, lambda wm: bench_commands(wm, runs, out))
    run("guard", outdir, out, lambda wm: bench_guard(wm, runs, out), "focus_guard enable\n")
    run("ipc", outdir, out, lambda wm: bench_ipc(wm, runs, out))
    run("layouts", outdir, out, lambda wm: bench_layouts(wm, out))
    run("frames", outdir, out, lambda wm: bench_frames(wm, outdir, out), "dim_inactive 0.12\n")
    run("flicker", outdir, out, lambda wm: bench_flicker(wm, outdir, max(runs // 2, 10), out))
    out["summary"] = summarize(out)
    with open(os.path.join(outdir, f"{MODE}-bench.json"), "w") as f:
        json.dump(out, f, indent=1)
    print(json.dumps(out["summary"], indent=1))


def summarize(out):
    s = {}
    for section in ("open_close", "commands", "guard", "flicker"):
        for row in out.get(section, []):
            if not isinstance(row, dict):
                continue
            for k, v in row.items():
                if isinstance(v, (int, float)) and not isinstance(v, bool) and k != "samples":
                    s.setdefault(f"{section}.{row['kind']}.{k}", []).append(v)
    for k, v in out.get("ipc", {}).items():
        s[f"ipc.{k}"] = v
    return {k: summary(v) for k, v in s.items()}


if __name__ == "__main__":
    main()
