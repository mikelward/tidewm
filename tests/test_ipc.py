"""tidewm's IPC, end to end.

Each test starts tidewm on wlroots' headless backend, opens windows with
tests/client, and checks what the sway-compatible socket reports. Nothing
sleeps: a test waits only for something it knows is coming (tidewm's
startup command, an event, a reply), and gives up after TIMEOUT seconds.
"""

import json
import os
import select
import shlex
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
    """tidewm on the headless backend, in a runtime directory of its own."""

    def __init__(self):
        self.dir = tempfile.TemporaryDirectory(prefix="tidewm-test-")
        os.chmod(self.dir.name, 0o700)
        self.clients = []
        ready = os.path.join(self.dir.name, "ready")
        os.mkfifo(ready)
        env = {k: v for k, v in os.environ.items()
               if k not in ("WAYLAND_DISPLAY", "DISPLAY", "SWAYSOCK", "I3SOCK")}
        env.update(XDG_RUNTIME_DIR=self.dir.name, WLR_BACKENDS="headless",
                   WLR_RENDERER="pixman", WLR_LIBINPUT_NO_DEVICES="1")
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

    def open_window(self, app_id, title, stdin=False):
        client = subprocess.Popen([CLIENT, app_id, title], env=self.env,
                                  stdin=subprocess.PIPE if stdin else subprocess.DEVNULL)
        self.clients.append(client)
        return client

    def open_x11_window(self, class_, title):
        client = subprocess.Popen([X11CLIENT, class_, title], env=self.env,
                                  stdin=subprocess.DEVNULL)
        self.clients.append(client)
        return client

    def stop(self):
        for client in self.clients:
            client.kill()
            client.wait(TIMEOUT)
        if self.proc.poll() is None:
            self.proc.terminate()
            self.proc.wait(TIMEOUT)
        self.log.close()
        self.dir.cleanup()


class IpcTest(unittest.TestCase):
    def setUp(self):
        self.wm = Tidewm()
        self.addCleanup(self.wm.stop)

    def connect(self):
        ipc = self.wm.ipc()
        self.addCleanup(ipc.close)
        return ipc

    def open_window(self, ipc, app_id, title):
        """Opens a window and returns its "new" event's container; ipc
        must be subscribed to window events."""
        client = self.wm.open_window(app_id, title)
        event = ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                               and e["container"]["app_id"] == app_id)
        return client, event["container"]

    def test_socket_is_named_in_swaysock(self):
        self.assertTrue(os.path.basename(self.wm.socket).startswith("tidewm-ipc."))
        self.assertEqual(os.path.dirname(self.wm.socket), self.wm.dir.name)

    def test_version_names_tidewm(self):
        version = self.connect().request(GET_VERSION)
        self.assertEqual(version["variant"], "tidewm")
        self.assertTrue(version["human_readable"].startswith("tidewm "))
        for field in ("major", "minor", "patch"):
            self.assertIsInstance(version[field], int)

    def test_outputs_list_the_headless_output(self):
        outputs = self.connect().request(GET_OUTPUTS)
        self.assertEqual(len(outputs), 1)
        output = outputs[0]
        self.assertEqual(output["name"], "HEADLESS-1")
        self.assertTrue(output["active"])
        self.assertTrue(output["focused"])
        self.assertGreater(output["rect"]["width"], 0)
        self.assertGreater(output["current_mode"]["height"], 0)

    def test_workspaces_are_empty_until_the_pool(self):
        self.assertEqual(self.connect().request(GET_WORKSPACES), [])

    def test_a_window_is_new_then_focused_then_closed(self):
        ipc = self.connect()
        ipc.subscribe("window")
        client, con = self.open_window(ipc, "test.lifecycle", "Lifecycle")
        self.assertEqual(con["name"], "Lifecycle")
        self.assertEqual(con["shell"], "xdg_shell")
        self.assertEqual(con["pid"], client.pid)
        self.assertRegex(con["foreign_toplevel_identifier"], r"^[0-9a-f]{32}$")
        focus = ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "focus")
        self.assertEqual(focus["container"]["id"], con["id"])
        self.assertTrue(focus["container"]["focused"])
        client.kill()
        client.wait(TIMEOUT)
        close = ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "close")
        self.assertEqual(close["container"]["id"], con["id"])

    def test_a_windows_first_event_is_new_once_it_is_placed(self):
        ipc = self.connect()
        ipc.subscribe("window")
        self.wm.open_window("test.placed", "Placed")
        kind, first = ipc.recv()
        self.assertEqual(kind, EVENT_WINDOW)
        self.assertEqual(first["change"], "new")
        self.assertEqual(first["container"]["app_id"], "test.placed")
        self.assertTrue(first["container"]["visible"])
        kind, second = ipc.recv()
        self.assertEqual((kind, second["change"]), (EVENT_WINDOW, "focus"))

    def test_a_window_that_maps_again_is_new_again(self):
        ipc = self.connect()
        ipc.subscribe("window")
        client = self.wm.open_window("test.remap", "Remap", stdin=True)
        new = ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new")
        ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "focus")
        client.stdin.write(b"remap\n")
        client.stdin.flush()
        close = ipc.next_event(EVENT_WINDOW)
        self.assertEqual((close["change"], close["container"]["id"]), ("close", new["container"]["id"]))
        again = ipc.next_event(EVENT_WINDOW)
        self.assertEqual((again["change"], again["container"]["id"]), ("new", new["container"]["id"]))

    def test_an_x11_window_shows_its_class_and_title(self):
        if not self.wm.x11:
            self.skipTest("tidewm wasn't built with XWayland, or has no Xwayland")
        ipc = self.connect()
        ipc.subscribe("window")
        self.wm.open_x11_window("test.x11", "Named")
        new = ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new")
        con = new["container"]
        self.assertEqual(con["shell"], "xwayland")
        self.assertIsNone(con["app_id"])
        self.assertEqual(con["window_properties"]["class"], "test.x11")
        self.assertEqual(con["name"], "Named")

    def test_tree_holds_windows_under_their_output(self):
        ipc = self.connect()
        ipc.subscribe("window")
        _, first = self.open_window(ipc, "test.first", "First")
        _, second = self.open_window(ipc, "test.second", "Second")
        tree = self.connect().request(GET_TREE)
        self.assertEqual(tree["type"], "root")
        output, = tree["nodes"]
        self.assertEqual(output["type"], "output")
        windows = {c["id"]: c for c in output["nodes"] + output["floating_nodes"]}
        self.assertEqual(set(windows), {first["id"], second["id"]})
        self.assertEqual(windows[second["id"]]["app_id"], "test.second")
        # dwl focuses the newest window
        self.assertTrue(windows[second["id"]]["focused"])
        self.assertFalse(windows[first["id"]]["focused"])

    def test_container_ids_differ_and_stay(self):
        ipc = self.connect()
        ipc.subscribe("window")
        _, first = self.open_window(ipc, "test.a", "A")
        _, second = self.open_window(ipc, "test.b", "B")
        self.assertNotEqual(first["id"], second["id"])
        tree = self.connect().request(GET_TREE)
        ids = {c["app_id"]: c["id"] for c in tree["nodes"][0]["nodes"]}
        self.assertEqual(ids, {"test.a": first["id"], "test.b": second["id"]})

    def test_an_unknown_command_fails(self):
        reply = self.connect().request(RUN_COMMAND, "frobnicate now")
        self.assertEqual(len(reply), 1)
        self.assertFalse(reply[0]["success"])
        self.assertTrue(reply[0]["parse_error"])
        self.assertIn("frobnicate", reply[0]["error"])

    def test_commands_run_in_order(self):
        reply = self.connect().request(RUN_COMMAND, "nop; frobnicate; nop")
        self.assertEqual([r["success"] for r in reply], [True, False, True])

    def test_subscribing_to_an_unknown_event_fails(self):
        reply = self.connect().request(SUBSCRIBE, '["window", "nope"]')
        self.assertFalse(reply["success"])

    def test_subscribing_needs_a_json_array(self):
        reply = self.connect().request(SUBSCRIBE, "window")
        self.assertFalse(reply["success"])

    def test_ticks_reach_subscribers(self):
        listener = self.connect()
        listener.subscribe("tick")
        first = listener.next_event(EVENT_TICK)
        self.assertEqual(first, {"first": True, "payload": ""})
        reply = self.connect().request(SEND_TICK, "hello")
        self.assertTrue(reply["success"])
        tick = listener.next_event(EVENT_TICK)
        self.assertEqual(tick, {"first": False, "payload": "hello"})

    def test_an_unsupported_message_fails_politely(self):
        reply = self.connect().request(5)  # GET_MARKS
        self.assertFalse(reply["success"])

    def test_bad_magic_drops_the_client_only(self):
        bad = self.connect()
        bad.sock.sendall(b"nope!!" + struct.pack("=II", 0, GET_VERSION))
        self.assertEqual(bad.sock.recv(1), b"")
        self.assertEqual(self.connect().request(GET_VERSION)["variant"], "tidewm")

    def test_exit_says_so_then_stops(self):
        listener = self.connect()
        listener.subscribe("shutdown")
        reply = self.connect().request(RUN_COMMAND, "exit")
        self.assertEqual(reply, [{"success": True}])
        self.assertEqual(listener.next_event(EVENT_SHUTDOWN), {"change": "exit"})
        self.assertEqual(self.wm.proc.wait(TIMEOUT), 0)
        self.assertFalse(os.path.exists(self.wm.socket))


if __name__ == "__main__":
    unittest.main()
