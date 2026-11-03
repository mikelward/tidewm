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
TIMEOUT = 10

MAGIC = b"i3-ipc"
HEADER = struct.Struct("=6sII")
RUN_COMMAND, GET_WORKSPACES, SUBSCRIBE, GET_OUTPUTS, GET_TREE = 0, 1, 2, 3, 4
GET_VERSION, SEND_TICK = 7, 10
EVENT_WINDOW = 0x80000003
EVENT_SHUTDOWN = 0x80000006
EVENT_TICK = 0x80000007


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
    with this many outputs."""

    def __init__(self, outputs=1):
        self.dir = tempfile.TemporaryDirectory(prefix="tidewm-test-")
        os.chmod(self.dir.name, 0o700)
        self.clients = []
        ready = os.path.join(self.dir.name, "ready")
        os.mkfifo(ready)
        env = {k: v for k, v in os.environ.items()
               if k not in ("WAYLAND_DISPLAY", "DISPLAY", "SWAYSOCK", "I3SOCK")}
        env.update(XDG_RUNTIME_DIR=self.dir.name, WLR_BACKENDS="headless",
                   WLR_RENDERER="pixman", WLR_LIBINPUT_NO_DEVICES="1",
                   WLR_HEADLESS_OUTPUTS=str(outputs))
        # The startup command runs once tidewm is up, with its sockets named
        # in its environment; writing them to the FIFO says it's ready.
        startup = (f'printf "%s %s %s\\n" "$SWAYSOCK" "$WAYLAND_DISPLAY" "${{DISPLAY:--}}"'
                   f' > {shlex.quote(ready)}')
        self.log = open(os.path.join(self.dir.name, "tidewm.log"), "w+")
        fifo = os.open(ready, os.O_RDONLY | os.O_NONBLOCK)
        self.proc = subprocess.Popen([TIDEWM, "-s", startup], env=env,
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

    def output(self):
        self.log.flush()
        self.log.seek(0)
        return self.log.read()

    def ipc(self):
        return Ipc(self.socket)

    def open_window(self, app_id, title, stdin=False, mapped_app_id=None):
        args = [app_id, title] + ([mapped_app_id] if mapped_app_id else [])
        client = subprocess.Popen([CLIENT, *args], env=self.env,
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
    """A test with its own tidewm; OUTPUTS says how many outputs it has."""

    OUTPUTS = 1

    def setUp(self):
        self.wm = Tidewm(self.OUTPUTS)
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

    def open_window(self, ipc, app_id, title=None):
        """Opens a window and returns its client and its "new" event's
        container; ipc must be subscribed to window events."""
        client = self.wm.open_window(app_id, title or app_id)
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
