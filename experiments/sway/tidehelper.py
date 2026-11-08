#!/usr/bin/env python3
"""tide's helper for stock sway: a prototype.

It owns the socket tide's clients see as SWAYSOCK and proxies sway's real
one. sway's own commands and requests pass through; tidewm's own ones
(grant, minimize, keep_open, layout tile, mfact, ...) it does itself, over
sway's IPC. It lays windows out itself: every tile is a sway floating
window it moves and sizes to tidewm's geometry, so the boxes are exact.

It is one process, one asyncio loop, Python's standard library only.

    tidehelper.py STATE_DIR

STATE_DIR holds state.json, written by tidewm-on-sway: the test config's
path, the startup command, and where the translated config goes.
"""

import asyncio
import json
import math
import os
import re
import shlex
import struct
import sys
import time
import traceback

MAGIC = b"i3-ipc"
HEADER = struct.Struct("=6sII")
RUN_COMMAND, GET_WORKSPACES, SUBSCRIBE, GET_OUTPUTS, GET_TREE = 0, 1, 2, 3, 4
GET_VERSION, GET_CONFIG, SEND_TICK = 7, 9, 10
EVENT_BIT = 0x80000000
EVENTS = {"workspace": 0, "output": 1, "mode": 2, "window": 3, "barconfig_update": 4,
          "binding": 5, "shutdown": 6, "tick": 7, "bar_state_update": 0x14, "input": 0x15,
          "layout": 0x1e, "capture": 0x1f}
EVENT_NAMES = {EVENT_BIT | v: k for k, v in EVENTS.items()}

LAYOUTS = ("tile", "threecol", "twocol", "monocle", "floating")
SYMBOLS = {"tile": "[]=", "floating": "><>", "monocle": "[M]", "threecol": "|M|",
           "twocol": "||="}
# tidewm's config.h and layout.h defaults
DEFAULTS = {"tile": (0.55, 1), "threecol": (0.5, 1), "twocol": (0.72, 2),
            "monocle": (0.55, 1), "floating": (0.55, 1)}
SINGLES = [(2.1, 0.8), (3.2, 0.6)]

# Lines of tidewm's config the helper takes; sway never sees them
OWN_SETTINGS = {"single_window", "default_layout", "ultrawide_aspect", "layout_defaults",
                "layout_cycle", "new_window", "dim_inactive", "xwayland_scaling",
                "resize_grab", "focus_guard", "rebind"}
# Commands that are tidewm's own: a binding running one goes through the helper
OWN_COMMANDS = {"grant", "persist", "unpersist", "minimize", "keep_open", "maximize", "nmaster",
                "mfact", "single_width", "zoom", "movestack", "dim", "send_shortcut",
                "switch", "gesture", "unlock", "kill", "floating", "layout", "focus",
                "move", "resize", "reload"}

TIMES = None
SETTLE_MS = float(os.environ.get("TIDE_SETTLE_MS", "200"))
LOG = None


def stamp(kind, con=None, **extra):
    """Writes a timestamp for experiments/sway/bench, on the same clock as
    Python's time.monotonic() in the bench."""
    if TIMES:
        TIMES.write(json.dumps(dict(ev=kind, con=con, t=time.monotonic(), **extra)) + "\n")
        TIMES.flush()


def log(*args):
    print("tidehelper:", *args, file=sys.stderr, flush=True)
    if LOG:
        print(time.strftime("%H:%M:%S"), *args, file=LOG, flush=True)


# --- config translation ----------------------------------------------------

def translate_config(text, settings):
    """tidewm's config for sway: tidewm's own lines go to settings, and a
    binding running a tidewm command becomes `nop tide CMD`, which the
    helper runs from the binding event."""
    out = []
    variables = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            out.append(raw)
            continue
        m = re.match(r"set\s+(\$\w+)\s+(.*)", line)
        if m:
            variables[m.group(1)] = m.group(2)
        for name in sorted(variables, key=len, reverse=True):
            line = line.replace(name, variables[name])
        word = line.split()[0]
        if word in OWN_SETTINGS:
            settings_line(line, settings)
            out.append("# tide: " + raw)
            continue
        if word in ("bindsym", "bindcode"):
            parts = line.split()
            flags = []
            i = 1
            while i < len(parts) and parts[i].startswith("--"):
                if parts[i] == "--name":
                    i += 2
                    continue
                if parts[i] != "--kept-open":
                    flags.append(parts[i])
                i += 1
            keys = parts[i] if i < len(parts) else ""
            command = line.split(None, i + 1)[i + 1] if len(parts) > i + 1 else ""
            if any(own_command(c) for c in split_commands(command)):
                command = "nop tide " + command
            out.append(" ".join([word, *flags, keys, command]))
            continue
        out.append(raw)
    if settings.get("focus_guard"):
        out.append("no_focus [all]")
    return "\n".join(out) + "\n"


def settings_line(line, settings):
    w = line.split()
    try:
        if w[0] == "focus_guard":
            if w[1] in ("enable", "disable"):
                settings["focus_guard"] = w[1] == "enable"
            elif w[1] == "grant_seconds":
                settings["grant_seconds"] = float(w[2])
        elif w[0] == "single_window":
            settings.setdefault("singles", []).append((float(w[1]), float(w[2])))
        elif w[0] == "default_layout":
            settings.setdefault("default_layout", {})[w[1]] = w[2]
        elif w[0] == "ultrawide_aspect":
            settings["ultrawide_aspect"] = float(w[1])
        elif w[0] == "layout_defaults":
            d = settings.setdefault("layout_defaults", {}).setdefault(w[1], {})
            for k, v in zip(w[2::2], w[3::2]):
                d[k] = float(v) if k == "mfact" else int(v)
        elif w[0] == "layout_cycle":
            settings["cycle"] = w[1:]
        elif w[0] == "new_window":
            settings["new_window"] = w[1]
        elif w[0] == "dim_inactive":
            settings["dim"] = float(w[1])
    except (IndexError, ValueError) as e:
        log(f"bad line {line!r}: {e}")


def split_commands(payload):
    """A RUN_COMMAND payload's commands, as tidewm splits them: on `;`
    outside double quotes, an exec taking the rest."""
    out, cur, quoted, i = [], "", False, 0
    while i < len(payload):
        ch = payload[i]
        if not quoted and ch == ";":
            out.append(cur)
            cur = ""
        else:
            if ch == '"':
                quoted = not quoted
            cur += ch
            stripped = re.sub(r"^\s*\[[^\]]*\]\s*", "", cur)
            if not quoted and stripped.strip() == "exec" and i + 1 < len(payload) and payload[i + 1] == " ":
                out.append(cur + payload[i + 1:])
                return [c.strip() for c in out if c.strip()]
        i += 1
    out.append(cur)
    return [c.strip() for c in out if c.strip()]


def criteria(command):
    """(con_id or None, other criteria text, the command after them)"""
    m = re.match(r"\s*\[([^\]]*)\]\s*(.*)", command, re.S)
    if not m:
        return None, "", command.strip()
    crit, rest = m.group(1), m.group(2)
    c = re.search(r"con_id=\"?(\d+)\"?", crit)
    other = re.sub(r"con_id=\"?\d+\"?", "", crit).strip()
    return (int(c.group(1)) if c else None), other, rest.strip()


def own_command(command):
    _, _, cmd = criteria(command)
    w = cmd.split()
    if not w:
        return False
    if w[0] == "layout":
        return len(w) > 1 and w[1] in LAYOUTS + ("toggle", "next", "prev")
    if w[0] == "focus":
        return len(w) > 1 and w[1] in ("recent", "left", "right", "up", "down", "next", "prev")
    if w[0] == "move":
        return len(w) > 1 and w[-1] in ("left", "right", "up", "down")
    if w[0] == "resize":
        return False
    return w[0] in OWN_COMMANDS - {"focus", "move", "resize", "reload", "layout"}


# --- geometry, after tidewm's layout.h -------------------------------------

def spans(start, length, weights):
    total = sum(weights)
    acc, prev, out = 0.0, start, []
    for w in weights:
        acc += w
        edge = math.floor(start + length * acc / total + 0.5)
        out.append((prev, edge - prev))
        prev = edge
    return out


def columns(area, weights):
    x, y, w, h = area
    return [(p, y, s, h) for p, s in spans(x, w, weights)]


def rows(area, n):
    x, y, w, h = area
    out, prev = [], y
    for i in range(n):
        edge = math.floor(y + h * (i + 1) / n + 0.5)
        out.append((x, prev, w, edge - prev))
        prev = edge
    return out


def single_width(area, rules, override):
    if override:
        return max(0.1, min(override, 1))
    aspect = area[2] / area[3] if area[3] else 0
    best, width = -1, 1
    for min_aspect, w in rules:
        if aspect >= min_aspect and min_aspect > best:
            best, width = min_aspect, w
    return max(0.1, min(width, 1))


def boxes_for(layout, area, n, mfact, nmaster, rules, single):
    if n == 0:
        return []
    if layout == "floating":
        return [None] * n
    if n == 1:
        f = single_width(area, rules, single)
        if f >= 1:
            return [area]
        side = (1 - f) / 2
        return [columns(area, [side, f, side])[1]]
    x, y, w, h = area
    if layout == "monocle":
        return [area] * n
    if layout == "tile":
        mw = round(w * mfact) if n > nmaster and nmaster else (0 if n > nmaster else w)
        out, my, ty = [], 0, 0
        for i in range(n):
            if i < nmaster:
                bh = (h - my) // (min(n, nmaster) - i)
                out.append((x, y + my, mw, bh))
                my += bh
            else:
                bh = (h - ty) // (n - i)
                out.append((x + mw, y + ty, w - mw, bh))
                ty += bh
        return out
    if layout == "threecol":
        nm = min(max(nmaster, 1), n)
        s = n - nm
        if s == 0:
            return rows(area, n)
        if s == 1:
            cols = columns(area, [mfact, 1 - mfact])
            return rows(cols[0], nm) + [cols[1]]
        cols = columns(area, [(1 - mfact) / 2, mfact, (1 - mfact) / 2])
        nright = (s + 1) // 2
        right, left = rows(cols[2], nright), rows(cols[0], s - nright)
        out = rows(cols[1], nm)
        r = l = 0
        for k in range(s):
            if k % 2 == 0:
                out.append(right[r]); r += 1
            else:
                out.append(left[l]); l += 1
        return out
    if layout == "twocol":
        nm = min(max(nmaster, 1), n, 15)
        weights = [1 if nm == n else mfact / nm] * nm
        if nm < n:
            weights.append(1 - mfact)
        cols = columns(area, weights)
        out = cols[:nm]
        if nm < n:
            out += rows(cols[nm], n - nm)
        return out
    return [area] * n


# --- the IPC ---------------------------------------------------------------

async def read_msg(reader):
    head = await reader.readexactly(HEADER.size)
    magic, length, kind = HEADER.unpack(head)
    if magic != MAGIC:
        raise ValueError("bad magic")
    return kind, await reader.readexactly(length)


def pack(kind, payload):
    data = payload if isinstance(payload, bytes) else json.dumps(payload).encode()
    return HEADER.pack(MAGIC, len(data), kind) + data


class Upstream:
    """One request/reply connection to sway, used one request at a time."""

    def __init__(self, path):
        self.path = path
        self.lock = asyncio.Lock()

    async def open(self):
        self.reader, self.writer = await asyncio.open_unix_connection(self.path)

    async def request(self, kind, payload=""):
        async with self.lock:
            self.writer.write(pack(kind, payload.encode() if isinstance(payload, str) else payload))
            await self.writer.drain()
            reply_kind, data = await read_msg(self.reader)
            return json.loads(data)

    async def command(self, command):
        stamp("cmd", cmd=command[:200])
        replies = await self.request(RUN_COMMAND, command)
        if isinstance(replies, list) and not all(r.get("success") for r in replies):
            log(f"sway refused {command[:300]!r}: {replies}")
        return replies


class Client:
    def __init__(self, reader, writer):
        self.reader, self.writer = reader, writer
        self.events = set()

    def send(self, kind, payload):
        if not self.writer.is_closing():
            self.writer.write(pack(kind, payload))


class Helper:
    def __init__(self, state_dir):
        with open(os.path.join(state_dir, "state.json")) as f:
            self.state = json.load(f)
        self.state_dir = state_dir
        self.real = os.environ["SWAYSOCK"]
        self.ctl = Upstream(self.real)
        self.clients = []
        self.settings = {}
        self.parts = {}           # workspace name -> part
        self.order = {}           # workspace name -> [con ids], stack order
        self.tiles = set()        # windows the helper tiles (sway floats them)
        self.adopting = set()     # floating events of our own to drop
        self.minimized = {}       # con -> workspace name
        self.kept = set()
        self.maximized = set()
        self.persistent = []
        self.urgent_seq = 0
        self.urgent_order = {}
        self.grants = []
        self.focus_history = []   # con ids, most recent last
        self.focused = None
        self.focused_ws = None
        self.granting = None
        self.syncs = {}
        self.sync_seq = 0
        self.last_layout = {}
        self.dimmed = {}
        self.tree = None
        self.info = {}            # con -> (app, pid, title)
        self.at = None

    # -- config

    def load_settings(self):
        self.settings = {}
        path = self.state["config"]
        text = ""
        if path and os.path.exists(path):
            with open(path) as f:
                text = f.read()
        self.config_text = text
        translated = translate_config(text, self.settings)
        with open(self.state["translated"], "w") as f:
            f.write(translated)

    def guard(self):
        return self.settings.get("focus_guard", False)

    # -- parts

    def part(self, ws, area):
        p = self.parts.get(ws)
        if p is None:
            aspect = area[2] / area[3] if area[3] else 0
            ultra = aspect >= self.settings.get("ultrawide_aspect", 2.1)
            dl = self.settings.get("default_layout", {})
            layout = dl.get("ultrawide" if ultra else "normal", "threecol" if ultra else "tile")
            p = {"layout": layout, "prev": "tile" if layout != "tile" else "threecol",
                 "modes": {}, "single": None}
            self.parts[ws] = p
        return p

    def mode(self, p, layout):
        if layout not in p["modes"]:
            mfact, nmaster = DEFAULTS[layout]
            d = self.settings.get("layout_defaults", {}).get(layout, {})
            p["modes"][layout] = [d.get("mfact", mfact), d.get("nmaster", nmaster)]
        return p["modes"][layout]

    # -- the tree

    async def get_tree(self):
        self.tree = await self.ctl.request(GET_TREE)
        return self.tree

    def walk(self, tree=None):
        """(window, workspace node, output node) for every window in sway's tree"""
        tree = tree or self.tree
        out = []

        def go(node, ws, output):
            if node.get("type") == "output":
                output = node
            if node.get("type") == "workspace":
                ws = node
            if node.get("type") in ("con", "floating_con") and not node["nodes"] and (
                    node.get("app_id") is not None or node.get("window") or node.get("pid")):
                out.append((node, ws, output))
            for c in node.get("nodes", []) + node.get("floating_nodes", []):
                go(c, ws, output)
        go(tree, None, None)
        return out

    def workspaces_in(self, tree):
        out = []
        for o in tree["nodes"]:
            for ws in o["nodes"]:
                if ws.get("type") == "workspace":
                    out.append((ws, o))
        return out

    # -- layout

    async def relayout(self):
        """Adopts sway's tiled windows as tiles and puts every tile where
        tidewm's layout has it, in one command so sway shows it at once."""
        tree = await self.get_tree()
        cmds = []
        wins = self.walk(tree)
        present = {w["id"] for w, _, _ in wins}
        for w, ws, out in wins:
            if ws is None or ws["name"].startswith("__i3"):
                continue
            self.info[w["id"]] = (app_of(w), w.get("pid"), w.get("name"))
            if w["type"] == "con" and w["id"] not in self.tiles:
                # sway tiled it: it's new, or tiled by sway itself
                self.tiles.add(w["id"])
                self.adopting.add(w["id"])
                cmds.append(f"[con_id={w['id']}] floating enable")
        for ws_name in list(self.order):
            self.order[ws_name] = [c for c in self.order[ws_name] if c in present]
        where = {}
        for w, ws, out in wins:
            if ws is not None and not ws["name"].startswith("__i3") and w["id"] in self.tiles:
                where[w["id"]] = (w, ws)
        # a tile on another workspace than its list's moved: it's new there
        for ws_name, lst in self.order.items():
            self.order[ws_name] = [c for c in lst if c in where and where[c][1]["name"] == ws_name]
        for cid, (w, ws) in where.items():
            lst = self.order.setdefault(ws["name"], [])
            if cid not in lst:
                self.place_new(ws["name"], lst, cid)
        wanted = {}
        for ws, out in self.workspaces_in(tree):
            if ws["name"].startswith("__i3"):
                continue
            # Moving a floating window, on any workspace, takes it to the
            # workspace its output shows (sway's container_floating_move_to),
            # so a hidden workspace is laid out once it's shown
            if not ws.get("visible") and not any(
                    o["current_workspace"] == ws["name"] for o in tree["nodes"]
                    if o.get("current_workspace")):
                continue
            r = ws["rect"]
            area = (r["x"], r["y"], r["width"], r["height"])
            p = self.part(ws["name"], area)
            lst = [c for c in self.order.get(ws["name"], [])
                   if c in where and not where[c][0].get("fullscreen_mode")]
            mfact, nmaster = self.mode(p, p["layout"])
            rules = self.settings.get("singles") or SINGLES
            boxes = boxes_for(p["layout"], area, len(lst), mfact, nmaster, rules, p["single"])
            for cid, box in zip(lst, boxes):
                if cid in self.maximized:
                    box = area
                if box is None:
                    continue
                wanted[cid] = box
                cur = where[cid][0]["rect"]
                if cid in self.adopting or (cur["x"], cur["y"], cur["width"], cur["height"]) != box:
                    x, y, bw, bh = box
                    cmds.append(f"[con_id={cid}] resize set width {bw} px height {bh} px,"
                                f" move absolute position {x} {y}")
        cmds += self.dim_commands(where)
        if cmds:
            stamp("layout_sent", n=len(cmds))
            replies = await self.ctl.command(";".join(cmds))
            stamp("layout_done")
            bad = [r for r in replies if not r.get("success")]
            if bad:
                log("layout:", bad, cmds)
            tree = await self.settle(wanted)
        await self.announce_layouts(tree)
        return tree

    async def settle(self, wanted):
        """sway sizes a floating window by what its client commits, so a
        tile has its box only once the client has drawn at that size. Waits
        for that, up to SETTLE_MS, so IPC clients see the layout done."""
        deadline = time.monotonic() + SETTLE_MS / 1000
        while True:
            tree = await self.get_tree()
            rects = {w["id"]: w["rect"] for w, ws, o in self.walk(tree)}
            off = [c for c, b in wanted.items() if c in rects and
                   (rects[c]["x"], rects[c]["y"], rects[c]["width"], rects[c]["height"]) != b]
            if not off:
                stamp("layout_settled")
                return tree
            if time.monotonic() > deadline:
                stamp("layout_unsettled", n=len(off))
                log(f"windows {off} didn't take their boxes in {SETTLE_MS} ms")
                return tree
            await asyncio.sleep(0.001)

    def place_new(self, ws, lst, cid):
        where = self.settings.get("new_window", "end")
        p = self.parts.get(ws)
        nmaster = self.mode(p, p["layout"])[1] if p else 1
        if where == "master":
            lst.insert(0, cid)
        elif where == "top":
            lst.insert(min(nmaster, len(lst)), cid)
        elif where == "next" and self.focused in lst:
            lst.insert(lst.index(self.focused) + 1, cid)
        else:
            lst.append(cid)

    def dim_commands(self, where):
        """Dims every window but the focused one with sway's opacity, over a
        black background: the nearest stock sway has to dim_inactive."""
        strength = self.settings.get("dim", 0)
        if not strength:
            return []
        out = []
        counts = {}
        for cid, (w, ws) in where.items():
            counts[ws["name"]] = counts.get(ws["name"], 0) + 1
        for cid, (w, ws) in where.items():
            want = 1.0 if (w.get("focused") or counts[ws["name"]] == 1) else round(1 - strength, 3)
            if self.dimmed.get(cid) != want:
                self.dimmed[cid] = want
                out.append(f"[con_id={cid}] opacity {want}")
        return out

    async def announce_layouts(self, tree):
        for o in tree["nodes"]:
            if o["name"].startswith("__i3") or not o.get("current_workspace"):
                continue
            ws = o["current_workspace"]
            p = self.parts.get(ws)
            if not p:
                continue
            key = (ws, p["layout"])
            if self.last_layout.get(o["name"]) != key:
                self.last_layout[o["name"]] = key
                self.emit("layout", {"change": "layout", "workspace": ws, "output": o["name"],
                                     "layout": p["layout"], "symbol": SYMBOLS[p["layout"]]})

    # -- events

    def emit(self, name, payload):
        kind = EVENT_BIT | EVENTS[name]
        for c in self.clients:
            if name in c.events:
                c.send(kind, payload)

    def enrich(self, con, tree=None):
        """A container as tidewm has it"""
        cid = con["id"]
        con = dict(con)
        if cid in self.tiles and cid not in self.minimized:
            con["type"] = "con"
        con["minimized"] = cid in self.minimized
        con["keep_open"] = cid in self.kept
        con["maximized"] = cid in self.maximized
        con["urgent_order"] = self.urgent_order.get(cid, 0) if con.get("urgent") else 0
        con["dim"] = True
        con["dimmed"] = self.dimmed.get(cid, 1.0) < 1.0
        if tree is not None:
            for w, ws, out in self.walk(tree):
                if w["id"] == cid:
                    con["workspace"] = self.minimized.get(cid, ws["name"] if ws else None)
                    con["output"] = out["name"] if out else None
                    con["rect"] = w["rect"]
                    con["focused"] = w["focused"]
                    con["visible"] = w.get("visible", False)
                    if cid not in self.tiles:
                        con["type"] = w["type"]
                    break
        if cid in self.minimized:
            con["workspace"] = self.minimized[cid]
            con["visible"] = False
        return con

    async def on_event(self, kind, event):
        name = EVENT_NAMES.get(kind)
        if name == "tick":
            payload = event.get("payload", "")
            if payload.startswith("tide-sync-"):
                fut = self.syncs.pop(payload, None)
                if fut:
                    fut.set_result(None)
                return
            self.emit("tick", event)
            return
        if name == "window":
            await self.on_window(event)
            return
        if name == "binding":
            cmd = event.get("binding", {}).get("command", "")
            if cmd.startswith("nop tide "):
                original = cmd[len("nop tide "):]
                event["binding"]["command"] = original
                self.cancel_grants("key")
                self.emit("binding", event)
                asyncio.ensure_future(self.run_commands(original, from_binding=True))
                return
            self.cancel_grants("key")
            self.emit("binding", event)
            return
        if name == "workspace":
            current = event.get("current") or {}
            if (current.get("name") or "").startswith("__i3"):
                return
            if event["change"] == "focus" and current:
                if self.focused_ws and event["current"]["name"] != self.focused_ws:
                    self.cancel_grants("workspace")
                self.focused_ws = event["current"]["name"]
            if event["change"] == "empty" and current and current["name"] not in self.persistent:
                self.parts.pop(current["name"], None)
                self.order.pop(current["name"], None)
            tree = await self.relayout()
            if event["change"] in ("init", "empty", "focus", "urgent", "rename"):
                for key in ("current", "old"):
                    if event.get(key):
                        event[key] = self.ws_info(event[key])
            if event["change"] == "rename" and current:
                self.rename(event)
            self.emit("workspace", event)
            return
        if name in ("output",):
            await self.relayout()
        if name:
            self.emit(name, event)

    def rename(self, event):
        new = event["current"]["name"]
        for g in self.grants:
            if g.get("ws_id") == event["current"].get("id"):
                g["made_ws"] = new

    def ws_info(self, ws):
        ws = dict(ws)
        ws["persistent"] = ws.get("name") in self.persistent
        p = self.parts.get(ws.get("name"))
        ws["layout"] = p["layout"] if p else None
        return ws

    async def on_window(self, event):
        change = event["change"]
        con = event["container"]
        cid = con["id"]
        if change == "new":
            stamp("new_recv", cid)
            await self.on_new(con)
            return
        if change == "floating" and cid in self.adopting:
            self.adopting.discard(cid)
            return
        if change == "focus":
            if cid != self.granting and cid != self.focused:
                self.cancel_grants("focus")
            self.focused = cid
            if cid in self.focus_history:
                self.focus_history.remove(cid)
            self.focus_history.append(cid)
            for m in list(self.maximized):
                if m != cid:
                    self.maximized.discard(m)
        if change == "urgent":
            if con.get("urgent"):
                g = self.grant_for(con, named_only=True)
                if g and cid != self.focused:
                    # an activation a grant names: let it in
                    g["used"] = True
                    self.granting = cid
                    await self.ctl.command(f"[con_id={cid}] focus")
                    return
                self.urgent_seq += 1
                self.urgent_order[cid] = self.urgent_seq
            else:
                self.urgent_order.pop(cid, None)
        if change == "close":
            self.tiles.discard(cid)
            self.kept.discard(cid)
            self.maximized.discard(cid)
            self.minimized.pop(cid, None)
            if cid in self.focus_history:
                self.focus_history.remove(cid)
            if cid == self.focused:
                self.focused = None
                self.cancel_grants("close")
        if change == "move" and cid in self.kept:
            self.kept.discard(cid)
        tree = await self.relayout()
        if change == "close":
            self.emit("window", event)
            return
        self.emit("window", dict(event, container=self.enrich(con, tree)))

    async def on_new(self, con):
        tree = await self.get_tree()
        cid = con["id"]
        found = [w for w, ws, out in self.walk(tree) if w["id"] == cid]
        w = found[0] if found else con
        here_ws = [ws for x, ws, out in self.walk(tree) if x["id"] == cid]
        here = here_ws[0]["name"] if here_ws and here_ws[0] else self.focused_ws
        app, pid, title = app_of(w), w.get("pid"), w.get("name")
        self.info[cid] = (app, pid, title)
        cmds = []
        popup = not app and not title
        g = None if popup else self.grant_for(w)
        prev = self.focused
        prev_app = self.info.get(prev, (None, None, None)) if prev else (None, None, None)
        same_app = prev is not None and (app and app == prev_app[0] or pid and pid == prev_app[1])
        target_ws = None
        if g and not g["canceled"]:
            give = True
            target_ws = g.get("workspace")
        elif g and g.get("places") and (g.get("workspace") or g["made_ws"] != self.focused_ws):
            give = False
            target_ws = g.get("workspace") or g["made_ws"]
        elif g:
            give = False
        elif self.guard():
            give = bool(same_app) and not popup
        else:
            give = True
        if target_ws and target_ws != here:
            cmds.append(f"[con_id={cid}] move to workspace {quote(target_ws)}")
        if give:
            if target_ws and target_ws != self.focused_ws:
                cmds.append(f"workspace {quote(target_ws)}")
            cmds.append(f"[con_id={cid}] focus")
            self.granting = cid
        else:
            if w.get("focused") and prev is not None:
                cmds.append(f"[con_id={prev}] focus")
                self.granting = prev
            if not popup and (self.guard() or target_ws):
                cmds.append(f"[con_id={cid}] urgent enable")
        if give and not g:
            self.cancel_grants("new")
        log(f"new {cid} app={app!r} on {here!r}, focused ws {self.focused_ws!r}, grant "
            f"{ {k: g[k] for k in ('apps', 'canceled', 'made_ws', 'workspace')} if g else None}: "
            f"{cmds}")
        if cmds:
            await self.ctl.command(";".join(cmds))
        tree = await self.relayout()
        stamp("new_settled", cid)
        self.emit("window", {"change": "new", "container": self.enrich(con, tree)})

    # -- grants

    def grant_for(self, w, named_only=False):
        app, pid = app_of(w), w.get("pid")
        now = time.monotonic()
        live = [g for g in self.grants if not g["used"] and now - g["t"] < g["seconds"]]
        for wildcard in (False, True):
            if wildcard and named_only:
                break
            for g in live:
                if g["wildcard"] != wildcard:
                    continue
                if g["wildcard"] and not g["pid"]:
                    return g
                if app and any(app_matches(a, app) for a in g["apps"]):
                    return g
                if g["pid"] and pid and descends(pid, g["pid"]):
                    return g
        return None

    def cancel_grants(self, why):
        for g in self.grants:
            if not g["canceled"]:
                g["canceled"] = True

    async def grant(self, args):
        try:
            words = shlex.split(args)
        except ValueError as e:
            return error(str(e))
        g = {"apps": [], "pid": None, "key": None, "workspace": None, "or_recent": False,
             "t": time.monotonic(), "used": False, "canceled": False,
             "seconds": self.settings.get("grant_seconds", 10), "made_ws": self.focused_ws}
        if words and words[0] == "extend":
            words = words[1:]
            opts, apps = parse_opts(words, {"--key", "--pid"})
            if opts is None or not opts.get("--key") or len(apps) < 2:
                return error("grant extend needs --key, an app and IDs")
            for x in self.grants:
                if x["key"] == opts["--key"] and x["apps"][:1] == apps[:1] and not x["used"]:
                    x["apps"] += apps[1:]
            return ok()
        if words and words[0] == "--or-recent":
            g["or_recent"] = True
            words = words[1:]
        opts, apps = parse_opts(words, {"--pid", "--key", "--workspace"})
        if opts is None:
            return error("grant: bad option")
        if "--pid" in opts:
            if not opts["--pid"].isdigit():
                return error("grant: --pid takes a number")
            g["pid"] = int(opts["--pid"])
        g["key"] = opts.get("--key")
        g["workspace"] = opts.get("--workspace")
        if not apps and not g["pid"]:
            return error("grant: name an app")
        if "*" in apps and len(apps) > 1:
            return error("grant: * stands alone")
        g["apps"] = [a for a in apps if a != "*"]
        g["wildcard"] = "*" in apps
        g["places"] = not g["or_recent"] and (not g["wildcard"] or g["pid"])
        self.grants = [x for x in self.grants if not x["used"]
                       and time.monotonic() - x["t"] < x["seconds"]]
        self.grants.append(g)
        if g["or_recent"]:
            asyncio.get_running_loop().call_later(g["seconds"], lambda: asyncio.ensure_future(
                self.lapse(g)))
        return ok()

    async def lapse(self, g):
        if g["used"] or g["canceled"]:
            return
        g["used"] = True
        await self.focus_recent(g["apps"])

    async def focus_recent(self, apps):
        for cid in reversed(self.focus_history):
            a = self.info.get(cid, (None,))[0]
            if a and any(app_matches(x, a) for x in apps):
                await self.focus(cid)
                return ok()
        for cid, (a, _, _) in self.info.items():
            if a and any(app_matches(x, a) for x in apps):
                await self.focus(cid)
                return ok()
        log(f"no window for {apps}")
        return error("no window of " + " ".join(apps))

    async def focus(self, cid):
        if cid in self.minimized:
            await self.unminimize(cid)
        await self.ctl.command(f"[con_id={cid}] focus")

    # -- commands

    async def run_commands(self, payload, from_binding=False):
        results = []
        for command in split_commands(payload):
            try:
                r = await self.own(command)
                if r is not None:
                    results.append(r)
                    continue
                r = await self.ctl.command(command)
                results += r
            except Exception as e:  # a prototype: say what broke, keep serving
                log(f"{command!r}: {e!r}")
                results.append(error(repr(e)))
        if not from_binding:
            await self.sync()
        return results

    async def sync(self):
        """Waits until the helper has handled every event sway sent before
        now, so a reply comes after the layout it caused."""
        self.sync_seq += 1
        key = f"tide-sync-{self.sync_seq}"
        fut = asyncio.get_running_loop().create_future()
        self.syncs[key] = fut
        await self.ctl.request(SEND_TICK, key)
        try:
            await asyncio.wait_for(fut, 5)
        except asyncio.TimeoutError:
            log(f"sync {key} timed out; the event loop is at {self.at!r}")
            self.syncs.pop(key, None)

    async def target(self, cid):
        """The window a command acts on: its criteria's, or the focused one"""
        if cid is not None:
            return cid
        tree = await self.get_tree()
        for w, ws, out in self.walk(tree):
            if w["focused"]:
                return w["id"]
        return None

    async def current_ws(self):
        wss = await self.ctl.request(GET_WORKSPACES)
        for ws in wss:
            if ws["focused"]:
                return ws
        return None

    async def own(self, command):
        """Runs a tidewm command, or returns None to pass it to sway"""
        cid, other, cmd = criteria(command)
        w = cmd.split()
        verb = w[0]
        args = cmd[len(verb):].strip()
        if verb == "reload":
            self.load_settings()
            self.parts_reset()
            return None
        if verb == "grant":
            return await self.grant(args)
        if verb in ("persist", "unpersist") and w[1:2] == ["workspace"]:
            name = unquote(cmd.split(None, 2)[2]) if len(w) > 2 else ""
            if not name:
                return error("name a workspace")
            if verb == "persist" and name not in self.persistent:
                self.persistent.append(name)
            if verb == "unpersist" and name in self.persistent:
                self.persistent.remove(name)
            return ok()
        if verb == "focus" and w[1:2] == ["recent"]:
            self.cancel_grants("focus recent")
            return await self.focus_recent(w[2:])
        if verb == "kill":
            t = await self.target(cid)
            if t in self.kept:
                self.emit("window", {"change": "close_refused",
                                     "container": self.enrich({"id": t}, await self.get_tree())})
                return error("the window is kept open")
            return None
        if verb == "keep_open":
            t = await self.target(cid)
            if t is None:
                return error("no window")
            on = toggle(args, t in self.kept)
            if on is None:
                return error("keep_open takes enable, disable or toggle")
            (self.kept.add if on else self.kept.discard)(t)
            self.emit("window", {"change": "keep_open",
                                 "container": self.enrich({"id": t}, await self.get_tree())})
            return ok()
        if verb == "minimize":
            t = await self.target(cid)
            if t is None:
                return error("no window")
            on = toggle(args or "toggle", t in self.minimized)
            if on is None:
                return error("minimize takes enable, disable or toggle")
            if on and t not in self.minimized:
                tree = await self.get_tree()
                ws = [ws for x, ws, o in self.walk(tree) if x["id"] == t]
                self.minimized[t] = ws[0]["name"] if ws and ws[0] else self.focused_ws
                for lst in self.order.values():
                    if t in lst:
                        lst.remove(t)
                await self.ctl.command(f"[con_id={t}] move scratchpad")
                await self.sync()
                self.emit("window", {"change": "minimize",
                                     "container": self.enrich({"id": t}, await self.relayout())})
            elif not on and t in self.minimized:
                await self.unminimize(t)
            return ok()
        if verb == "maximize":
            t = await self.target(cid)
            on = toggle(args or "toggle", t in self.maximized)
            if t is None or on is None:
                return error("maximize: no window, or a bad argument")
            (self.maximized.add if on else self.maximized.discard)(t)
            await self.ctl.command(f"[con_id={t}] focus")
            tree = await self.relayout()
            self.emit("window", {"change": "maximize", "container": self.enrich({"id": t}, tree)})
            return ok()
        if verb == "floating":
            t = await self.target(cid)
            if t is None:
                return error("no window")
            tiled = t in self.tiles
            on = toggle(args, not tiled)
            if on is None:
                return error("floating takes enable, disable or toggle")
            if on and tiled:
                self.tiles.discard(t)
                for lst in self.order.values():
                    if t in lst:
                        lst.remove(t)
                await self.ctl.command(f"[con_id={t}] move position center")
            elif not on and not tiled:
                self.tiles.add(t)
                await self.ctl.command(f"[con_id={t}] floating enable")
            else:
                return ok()
            tree = await self.relayout()
            self.emit("window", {"change": "floating", "container": self.enrich({"id": t}, tree)})
            return ok()
        if verb == "layout":
            if len(w) < 2 or w[1] not in LAYOUTS + ("toggle", "next", "prev"):
                return None
            ws = await self.current_ws()
            r = ws["rect"]
            p = self.part(ws["name"], (r["x"], r["y"], r["width"], r["height"]))
            cycle = self.settings.get("cycle") or ["tile", "threecol", "twocol"]
            cur = p["layout"]
            if w[1] == "toggle":
                new = p["prev"]
            elif w[1] in ("next", "prev"):
                base = p["prev"] if cur == "monocle" else cur
                if base in cycle:
                    new = cycle[(cycle.index(base) + (1 if w[1] == "next" else -1)) % len(cycle)]
                else:
                    new = cycle[0 if w[1] == "next" else -1]
            else:
                new = w[1]
            if new != cur:
                p["prev"], p["layout"] = cur, new
            await self.relayout()
            return ok()
        if verb in ("mfact", "nmaster", "single_width"):
            ws = await self.current_ws()
            r = ws["rect"]
            p = self.part(ws["name"], (r["x"], r["y"], r["width"], r["height"]))
            m = self.mode(p, p["layout"])
            arg = args.strip()
            try:
                if verb == "single_width":
                    if arg == "reset":
                        p["single"] = None
                    else:
                        f = float(arg)
                        if not 0.1 <= f <= 1:
                            return error("single_width is from 0.1 to 1")
                        p["single"] = f
                elif verb == "mfact":
                    f = float(arg)
                    if not math.isfinite(f):
                        return error("mfact must be finite")
                    f = m[0] + f if arg[0] in "+-" else f
                    if not 0.1 <= f <= 0.9:
                        return error("mfact is from 0.1 to 0.9")
                    m[0] = f
                else:
                    n = int(arg)
                    m[1] = max(0, m[1] + n if arg[0] in "+-" else n)
            except ValueError:
                return error(f"{verb}: bad number")
            await self.relayout()
            return ok()
        if verb in ("zoom", "movestack"):
            t = await self.target(cid)
            lst = next((l for l in self.order.values() if t in l), None)
            if lst is None:
                return error("not a tile")
            i = lst.index(t)
            if verb == "zoom":
                j = 1 if i == 0 and len(lst) > 1 else 0
            else:
                if args not in ("next", "prev"):
                    return error("movestack takes next or prev")
                j = (i + (1 if args == "next" else -1)) % len(lst)
            if verb == "zoom":
                # dwl's zoom: the window goes to the top, the rest move down
                lst.insert(0, lst.pop(j if i == 0 else i))
            else:
                lst[i], lst[j] = lst[j], lst[i]
            if verb == "zoom" and i == 0 and len(lst) > 1:
                await self.ctl.command(f"[con_id={lst[0]}] focus")
            await self.relayout()
            return ok()
        if verb == "focus" and len(w) > 1 and w[1] in ("next", "prev"):
            tree = await self.get_tree()
            ws = await self.current_ws()
            lst = [c for c in self.order.get(ws["name"], [])]
            if not lst:
                return error("no window")
            i = lst.index(self.focused) if self.focused in lst else -1
            j = (i + (1 if w[1] == "next" else -1)) % len(lst)
            await self.focus(lst[j])
            return ok()
        if verb in ("focus", "move") and len(w) > 1 and w[-1] in ("left", "right", "up", "down"):
            if verb == "move" and len(w) > 2 and w[-2] not in ("move", "container", "window", "to"):
                return None
            t = await self.target(cid)
            if t not in self.tiles:
                return None
            tree = await self.get_tree()
            nxt = pick_direction(tree, self.walk(tree), t, w[-1], self.tiles)
            if nxt is None:
                return ok()
            if verb == "focus":
                await self.focus(nxt)
            else:
                lst = next((l for l in self.order.values() if t in l), None)
                if lst is None or nxt not in lst:
                    return ok()
                i, j = lst.index(t), lst.index(nxt)
                lst[i], lst[j] = lst[j], lst[i]
                await self.relayout()
            return ok()
        if verb == "focus" and cid is not None and len(w) == 1:
            if cid in self.minimized:
                await self.unminimize(cid)
            self.cancel_grants("focus")
            return None
        if verb == "resize" and len(w) >= 3 and w[1] in ("grow", "shrink") and \
                w[2] in ("width", "horizontal", "left", "right"):
            t = await self.target(cid)
            if t not in self.tiles:
                return None
            ws = await self.current_ws()
            p = self.parts.get(ws["name"])
            if not p:
                return error("no layout")
            m = self.mode(p, p["layout"])
            lst = self.order.get(ws["name"], [])
            n = float(w[3]) if len(w) > 3 and w[3].replace(".", "").isdigit() else 10
            delta = n / 100 * (1 if w[1] == "grow" else -1)
            if lst.index(t) >= m[1]:
                delta = -delta
            f = m[0] + delta
            if not 0.1 <= f <= 0.9:
                return error("can't go further")
            m[0] = f
            await self.relayout()
            return ok()
        if verb == "urgent" and args in ("enable", "disable"):
            t = await self.target(cid)
            tree = await self.get_tree()
            w = next((x for x, ws, o in self.walk(tree) if x["id"] == t), None)
            if w is not None and args == "enable" and w.get("urgent"):
                self.urgent_seq += 1
                self.urgent_order[t] = self.urgent_seq
                self.emit("window", {"change": "urgent", "container": self.enrich(w, tree)})
                return ok()
            return None
        if verb in ("dim",):
            return ok()
        if verb in ("send_shortcut", "switch", "gesture", "unlock"):
            return error(f"{verb} isn't possible over sway's IPC")
        return None

    def parts_reset(self):
        for p in self.parts.values():
            p["modes"] = {}

    async def unminimize(self, t):
        ws = self.minimized.pop(t)
        await self.ctl.command(f"[con_id={t}] scratchpad show; [con_id={t}] move to workspace "
                               f"{quote(ws)}; [con_id={t}] floating enable")
        self.order.setdefault(ws, []).append(t)
        await self.sync()
        tree = await self.relayout()
        self.emit("window", {"change": "minimize", "container": self.enrich({"id": t}, tree)})

    # -- requests

    async def handle_request(self, client, kind, data):
        text = data.decode(errors="replace")
        if kind == RUN_COMMAND:
            return await self.run_commands(text)
        if kind == SUBSCRIBE:
            try:
                names = json.loads(text)
                if not isinstance(names, list) or any(n not in EVENTS for n in names):
                    raise ValueError
            except ValueError:
                return {"success": False}
            client.events.update(names)
            client.send(SUBSCRIBE, {"success": True})
            if "tick" in names:
                client.send(EVENT_BIT | EVENTS["tick"], {"first": True, "payload": ""})
            return None
        if kind == GET_TREE:
            return self.rewrite_tree(await self.get_tree())
        if kind == GET_WORKSPACES:
            wss = [self.ws_info(w) for w in await self.ctl.request(GET_WORKSPACES)
                   if not w["name"].startswith("__i3")]
            names = {w["name"] for w in wss}
            for name in self.persistent:
                if name not in names:
                    wss.append({"id": 0, "name": name, "num": int(name) if name.isdigit() else -1,
                                "focused": False, "visible": False, "urgent": False,
                                "persistent": True, "output": wss[0]["output"] if wss else "",
                                "rect": wss[0]["rect"] if wss else {},
                                "layout": (self.parts.get(name) or {}).get("layout")})
            return wss
        if kind == GET_CONFIG:
            return {"config": self.config_text, "included_configs": [],
                    "path": self.state["config"], "errors": []}
        return await self.ctl.request(kind, data)

    def rewrite_tree(self, tree):
        tree = json.loads(json.dumps(tree))
        mins = []
        tree["nodes"] = [o for o in tree["nodes"] if not o["name"].startswith("__i3")
                         or mins.extend(scratch(o))]
        for o in tree["nodes"]:
            for ws in o["nodes"]:
                floats = ws.get("floating_nodes", [])
                tiles = {c["id"]: c for c in floats if c["id"] in self.tiles}
                ws["floating_nodes"] = [c for c in floats if c["id"] not in self.tiles]
                ordered = [tiles.pop(c) for c in self.order.get(ws["name"], []) if c in tiles]
                ws["nodes"] = ws["nodes"] + ordered + list(tiles.values())
                for c in ws["nodes"] + ws["floating_nodes"]:
                    self.fix(c, ws["name"], o["name"])
                for m in mins:
                    if self.minimized.get(m["id"]) == ws["name"]:
                        self.fix(m, ws["name"], o["name"])
                        m["type"] = "con" if m["id"] in self.tiles else "floating_con"
                        m["visible"] = False
                        ws["nodes"].append(m)
        return tree

    def fix(self, c, ws, out):
        if c["id"] in self.tiles:
            c["type"] = "con"
        c["minimized"] = c["id"] in self.minimized
        c["keep_open"] = c["id"] in self.kept
        c["maximized"] = c["id"] in self.maximized
        c["urgent_order"] = self.urgent_order.get(c["id"], 0) if c.get("urgent") else 0
        c["dim"] = True
        c["dimmed"] = self.dimmed.get(c["id"], 1.0) < 1.0
        c["workspace"], c["output"] = ws, out
        for k in c.get("nodes", []) + c.get("floating_nodes", []):
            self.fix(k, ws, out)

    # -- serving

    async def serve_client(self, reader, writer):
        client = Client(reader, writer)
        self.clients.append(client)
        try:
            while True:
                kind, data = await read_msg(reader)
                try:
                    reply = await self.handle_request(client, kind, data)
                except Exception as e:
                    log(f"request {kind} {data[:200]!r}: {traceback.format_exc()}")
                    reply = [error(repr(e))] if kind == RUN_COMMAND else {"success": False,
                                                                       "error": repr(e)}
                if reply is not None:
                    client.send(kind, reply)
                await writer.drain()
        except (asyncio.IncompleteReadError, ConnectionError, ValueError):
            pass
        finally:
            self.clients.remove(client)
            writer.close()

    async def subscribe(self):
        """Subscribes before anything is served, so no sync's tick is missed"""
        r, w = await asyncio.open_unix_connection(self.real)
        names = ["workspace", "output", "mode", "window", "binding", "shutdown", "tick", "input"]
        w.write(pack(SUBSCRIBE, json.dumps(names).encode()))
        await w.drain()
        while True:
            kind, data = await read_msg(r)
            if kind == SUBSCRIBE:
                break
        self.ev = (r, w)

    async def events(self):
        r, w = self.ev
        while True:
            try:
                kind, data = await read_msg(r)
            except (asyncio.IncompleteReadError, ConnectionError, ValueError) as e:
                log(f"sway went: {e!r}")
                os._exit(0)
            if kind == SUBSCRIBE:
                continue
            try:
                self.at = (kind, data[:300])
                await self.on_event(kind, json.loads(data))
                self.at = None
            except Exception:
                log(f"event {kind:#x}: {traceback.format_exc()}")

    async def main(self):
        global TIMES, LOG
        if os.environ.get("TIDE_HELPER_LOG"):
            LOG = open(os.environ["TIDE_HELPER_LOG"], "a")
            print(f"--- helper {os.getpid()} for {self.state['config']}", file=LOG, flush=True)
        TIMES = open(os.path.join(self.state_dir, "helper-times.jsonl"), "a")
        self.load_settings()
        await self.ctl.open()
        await self.subscribe()
        asyncio.ensure_future(self.events())
        path = os.path.join(os.environ["XDG_RUNTIME_DIR"],
                            f"tidewm-ipc.{os.getuid()}.{os.getpid()}.sock")
        server = await asyncio.start_unix_server(self.serve_client, path)
        await self.relayout()
        for ws in await self.ctl.request(GET_WORKSPACES):
            if ws["focused"]:
                self.focused_ws = ws["name"]
        env = dict(os.environ, SWAYSOCK=path, I3SOCK=path)
        await asyncio.create_subprocess_shell(self.state["startup"], env=env)
        async with server:
            await server.serve_forever()


def scratch(output):
    out = []
    for ws in output["nodes"]:
        out += ws.get("floating_nodes", []) + ws.get("nodes", [])
    return out


def pick_direction(tree, wins, t, d, tiles):
    """tidewm's rule for arrow keys: of the windows whose center is that way,
    the one sharing the most with the focused one, then the nearest."""
    rects = {w["id"]: w["rect"] for w, ws, o in wins if w.get("visible") and w["id"] in tiles}
    if t not in rects:
        return None
    a = rects[t]
    ac = (a["x"] + a["width"] / 2, a["y"] + a["height"] / 2)
    best = None
    for cid, b in rects.items():
        if cid == t:
            continue
        bc = (b["x"] + b["width"] / 2, b["y"] + b["height"] / 2)
        dx, dy = bc[0] - ac[0], bc[1] - ac[1]
        if not {"left": dx < 0, "right": dx > 0, "up": dy < 0, "down": dy > 0}[d]:
            continue
        if d in ("left", "right"):
            share = min(a["y"] + a["height"], b["y"] + b["height"]) - max(a["y"], b["y"])
        else:
            share = min(a["x"] + a["width"], b["x"] + b["width"]) - max(a["x"], b["x"])
        key = (-max(share, 0), abs(dx) + abs(dy))
        if best is None or key < best[0]:
            best = (key, cid)
    return best[1] if best else None


def app_of(w):
    app = w.get("app_id")
    if app is None:
        app = (w.get("window_properties") or {}).get("class")
    return app or ""


def app_matches(name, app):
    norm = lambda s: re.sub(r"\.desktop$", "", s.lower())
    name, app = norm(name), norm(app)
    return name == app or ("." not in name and app.rsplit(".", 1)[-1] == name)


def descends(pid, ancestor):
    for _ in range(64):
        if pid == ancestor:
            return True
        try:
            with open(f"/proc/{pid}/stat") as f:
                pid = int(f.read().rsplit(")", 1)[1].split()[1])
        except (OSError, ValueError, IndexError):
            return False
        if pid <= 1:
            return pid == ancestor
    return False


def parse_opts(words, known):
    opts, rest, i = {}, [], 0
    while i < len(words):
        word = words[i]
        if word.startswith("--"):
            name, eq, value = word.partition("=")
            if name not in known:
                return None, None
            if not eq:
                i += 1
                if i >= len(words):
                    return None, None
                value = words[i]
            opts[name] = value
        else:
            rest.append(word)
        i += 1
    return opts, rest


def toggle(arg, now):
    return {"enable": True, "disable": False, "toggle": not now}.get(arg.strip())


def quote(name):
    return '"' + name + '"'


def unquote(s):
    s = s.strip()
    return s[1:-1] if len(s) > 1 and s[0] == s[-1] == '"' else s


def ok():
    return {"success": True}


def error(msg):
    return {"success": False, "error": msg}


if __name__ == "__main__":
    asyncio.run(Helper(sys.argv[1]).main())
