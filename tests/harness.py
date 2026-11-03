"""What tidewm's tests share: tidewm on wlroots' headless backend, a client
for its IPC, and tests/client for windows (tests/x11client for X11 ones).

Nothing sleeps: a test waits only for something it knows is coming
(tidewm's startup command, an event, a reply), and gives up after TIMEOUT
seconds.
"""

import json
import os
import select
import shlex
import shutil
import socket
import struct
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TIDEWM = os.environ.get("TIDEWM", os.path.join(ROOT, "dwl"))
CLIENT = os.path.join(ROOT, "tests", "client")
X11CLIENT = os.path.join(ROOT, "tests", "x11client")
LOCKER = os.path.join(ROOT, "tests", "locker")
TIMEOUT = 10

MAGIC = b"i3-ipc"
HEADER = struct.Struct("=6sII")
RUN_COMMAND, GET_WORKSPACES, SUBSCRIBE, GET_OUTPUTS, GET_TREE = 0, 1, 2, 3, 4
GET_VERSION, GET_BINDING_MODES, GET_CONFIG, SEND_TICK, GET_BINDING_STATE = 7, 8, 9, 10, 12
GET_INPUTS = 100
EVENT_WORKSPACE = 0x80000000
EVENT_MODE = 0x80000002
EVENT_WINDOW = 0x80000003
EVENT_SHUTDOWN = 0x80000006
EVENT_TICK = 0x80000007
EVENT_INPUT = 0x80000015


def is_urgency(event):
    """Whether a workspace event is a change of urgency."""
    return event["change"] == "urgent"


def urgency(event):
    """A workspace event's workspace and whether it's urgent."""
    return event["current"]["name"], event["current"]["urgent"]


class Ipc:
    """One connection to tidewm's socket."""

    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(TIMEOUT)
        self.sock.connect(path)

    def close(self):
        self.sock.close()

    def send(self, kind, payload=""):
        data = payload.encode()
        self.sock.sendall(HEADER.pack(MAGIC, len(data), kind) + data)

    def _read(self, n):
        data = b""
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise EOFError("tidewm closed the connection")
            data += chunk
        return data

    def recv(self):
        magic, length, kind = HEADER.unpack(self._read(HEADER.size))
        if magic != MAGIC:
            raise ValueError(f"bad magic {magic!r}")
        return kind, json.loads(self._read(length))

    def request(self, kind, payload=""):
        self.send(kind, payload)
        reply_kind, reply = self.recv()
        if reply_kind != kind:
            raise AssertionError(f"asked {kind}, got a reply of type {reply_kind:#x}")
        return reply

    def subscribe(self, *events):
        reply = self.request(SUBSCRIBE, json.dumps(list(events)))
        if not reply.get("success"):
            raise AssertionError(f"subscribing to {events} failed: {reply}")

    def next_event(self, kind, match=lambda e: True):
        """The next event of this kind that match accepts; others are skipped."""
        while True:
            event_kind, event = self.recv()
            if event_kind == kind and match(event):
                return event


class Tidewm:
    """tidewm on the headless backend, in a runtime directory of its own,
    with this many outputs. With config, it has that config file: named
    with -c if config_arg, else at its default place under
    XDG_CONFIG_HOME, which is in the runtime directory either way, so the
    user's own config never leaks in. With parent, another Tidewm, it runs
    nested in that one instead, on wlroots' Wayland backend, which gives it
    a keyboard of its own if the parent's seat has one, as a real keyboard
    would be, rather than a client's virtual one."""

    def __init__(self, outputs=1, config=None, config_arg=True, parent=None):
        self.dir = tempfile.TemporaryDirectory(prefix="tidewm-test-")
        os.chmod(self.dir.name, 0o700)
        self.clients = []
        ready = os.path.join(self.dir.name, "ready")
        os.mkfifo(ready)
        env = {k: v for k, v in os.environ.items()
               if k not in ("WAYLAND_DISPLAY", "DISPLAY", "SWAYSOCK", "I3SOCK")}
        env.update(XDG_RUNTIME_DIR=self.dir.name, WLR_BACKENDS="headless",
                   WLR_RENDERER="pixman", WLR_LIBINPUT_NO_DEVICES="1",
                   WLR_HEADLESS_OUTPUTS=str(outputs),
                   XDG_CONFIG_HOME=os.path.join(self.dir.name, "xdg"))
        if parent:
            env.update(WLR_BACKENDS="wayland", WLR_WL_OUTPUTS=str(outputs),
                       WAYLAND_DISPLAY=os.path.join(parent.dir.name,
                                                    parent.env["WAYLAND_DISPLAY"]))
        args = []
        if config_arg:
            self.config_path = os.path.join(self.dir.name, "config")
            if config is not None:
                args = ["-c", self.config_path]
        else:
            self.config_path = os.path.join(self.dir.name, "xdg", "tidewm", "config")
        if config is not None:
            self.write_config(config)
        # The startup command runs once tidewm is up, with its sockets named
        # in its environment; writing them to the FIFO says it's ready.
        startup = (f'printf "%s %s %s\\n" "$SWAYSOCK" "$WAYLAND_DISPLAY" "${{DISPLAY:--}}"'
                   f' > {shlex.quote(ready)}')
        self.log = open(os.path.join(self.dir.name, "tidewm.log"), "w+")
        fifo = os.open(ready, os.O_RDONLY | os.O_NONBLOCK)
        self.proc = subprocess.Popen([TIDEWM, *args, "-s", startup], env=env,
                                     stdout=subprocess.DEVNULL, stderr=self.log)
        line = b""
        try:
            while not line.endswith(b"\n"):
                if not select.select([fifo], [], [], TIMEOUT)[0]:
                    raise RuntimeError(f"tidewm didn't start:\n{self.output()}")
                chunk = os.read(fifo, 4096)
                if not chunk:
                    raise RuntimeError(f"tidewm's startup command wrote nothing:\n{self.output()}")
                line += chunk
        finally:
            os.close(fifo)
        self.socket, display, x11 = line.decode().split()
        self.env = dict(env, WAYLAND_DISPLAY=display)
        # DISPLAY is set only when tidewm was built with XWayland and
        # found Xwayland.
        self.x11 = x11 != "-"
        if self.x11:
            self.env["DISPLAY"] = x11

    def write_config(self, text):
        os.makedirs(os.path.dirname(self.config_path), exist_ok=True)
        with open(self.config_path, "w") as f:
            f.write(text)

    def output(self):
        self.log.flush()
        self.log.seek(0)
        return self.log.read()

    def ipc(self):
        return Ipc(self.socket)

    def open_window(self, app_id, title, stdin=False, mapped_app_id=None, min_size=None,
                    max_size=None):
        args = [app_id, title] + ([mapped_app_id] if mapped_app_id else [])
        env = dict(self.env)
        if min_size:
            env["MIN_SIZE"] = min_size
        if max_size:
            env["MAX_SIZE"] = max_size
        client = subprocess.Popen([CLIENT, *args], env=env,
                                  stdin=subprocess.PIPE if stdin else subprocess.DEVNULL)
        self.clients.append(client)
        return client

    def open_x11_window(self, class_, title):
        client = subprocess.Popen([X11CLIENT, class_, title], env=self.env,
                                  stdin=subprocess.DEVNULL)
        self.clients.append(client)
        return client

    def press(self, *args):
        """Types with wtype, over the virtual keyboard protocol: args are
        wtype's, such as "-M", "alt", "-k", "period"."""
        subprocess.run(["wtype", *args], env=self.env, check=True, timeout=TIMEOUT)

    def stop(self):
        for client in self.clients:
            client.kill()
            client.wait(TIMEOUT)
            if client.stdin:
                client.stdin.close()
        if self.proc.poll() is None:
            self.proc.terminate()
            self.proc.wait(TIMEOUT)
        self.log.close()
        self.dir.cleanup()


class TidewmTest(unittest.TestCase):
    """A test with its own tidewm. OUTPUTS says how many outputs it has;
    CONFIG and CONFIG_ARG, its config file, as Tidewm takes them."""

    OUTPUTS = 1
    CONFIG = None
    CONFIG_ARG = True

    def setUp(self):
        self.wm = Tidewm(self.OUTPUTS, self.CONFIG, self.CONFIG_ARG)
        self.addCleanup(self.wm.stop)

    def need(self, ok, why):
        """Skips the test unless ok, or fails it in CI, which has
        everything, so that nothing there skips unseen."""
        if not ok:
            if os.environ.get("CI"):
                self.fail(why)
            self.skipTest(why)

    def press(self, *args):
        self.need(shutil.which("wtype"), "typing keys needs wtype")
        self.wm.press(*args)

    def connect(self):
        ipc = self.wm.ipc()
        self.addCleanup(ipc.close)
        return ipc

    def open_window(self, ipc, app_id, title=None, min_size=None, max_size=None):
        """Opens a window, at least min_size and at most max_size ("WxH")
        big where they're given, and returns its client and its "new"
        event's container; ipc must be subscribed to window events."""
        client = self.wm.open_window(app_id, title or app_id, min_size=min_size,
                                     max_size=max_size)
        event = ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                               and e["container"]["app_id"] == app_id)
        return client, event["container"]

    def command(self, ipc, command):
        """Runs one command, asserting that it worked."""
        reply = ipc.request(RUN_COMMAND, command)
        self.assertEqual(reply, [{"success": True}], f"{command!r}: {reply}")

    def windows(self):
        """Every window in the tree, by id."""
        found = {}

        def walk(node):
            if node.get("type") in ("con", "floating_con"):
                found[node["id"]] = node
            for child in node.get("nodes", []) + node.get("floating_nodes", []):
                walk(child)

        walk(self.connect().request(GET_TREE))
        return found

    def workspace_names(self):
        return [w["name"] for w in self.connect().request(GET_WORKSPACES)]

    def urgent_window_on(self, ipc, workspace):
        """Opens a window on a persistent workspace, not the current one,
        and has it ask to be activated, which makes it urgent. ipc must be
        subscribed to window and workspace events."""
        client = self.wm.open_window("test.urgent", "Urgent", stdin=True)
        con = ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new")["container"]
        self.command(self.connect(), f"persist workspace {workspace}")
        self.command(self.connect(), f"[con_id={con['id']}] move to workspace {workspace}")
        client.stdin.write(b"activate\n")
        client.stdin.flush()
        self.assertEqual(urgency(ipc.next_event(EVENT_WORKSPACE, is_urgency)), (workspace, True))
        return client, con


def ext_workspaces(wm, *args):
    """What tests/workspaces prints: each ext-workspace workspace as
    (name, state, outputs its group is on)."""
    out = subprocess.run([os.path.join(ROOT, "tests", "workspaces"), *args],
                         env=wm.env, capture_output=True, text=True,
                         timeout=TIMEOUT, check=True).stdout
    return [(name, state, int(outputs))
            for name, state, outputs in (line.rsplit(" ", 2) for line in out.splitlines())]


def power(wm, output, state):
    """Turns an output on or off with tests/power, as an idle daemon would,
    over wlr-output-power-management."""
    out = subprocess.run([os.path.join(ROOT, "tests", "power"), output, state],
                         env=wm.env, capture_output=True, text=True,
                         timeout=TIMEOUT, check=True).stdout
    assert out == f"{state}\n", out


def randr(test, *args):
    """What wlr-randr says, over wlr-output-management, as display settings
    tools see the outputs."""
    test.need(shutil.which("wlr-randr"), "output management needs wlr-randr")
    return subprocess.run(["wlr-randr", *args], env=test.wm.env, capture_output=True,
                          text=True, timeout=TIMEOUT, check=True).stdout
