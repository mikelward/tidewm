"""Touchpad gestures, over pointer-gestures: a pinch, swipe or hold goes to
the window under the pointer. The headless backend has no touchpad, so the
IPC's gesture command makes each one, as if from a touchpad, on the
cursor's events, and tests/client says what it got."""

import os
import select
import subprocess
import unittest

from harness import EVENT_WINDOW, GET_OUTPUTS, ROOT, RUN_COMMAND, TIMEOUT, TidewmTest

POINTER = os.path.join(ROOT, "tests", "pointer")


class GestureTest(TidewmTest):
    def line(self, client, start):
        """The client's next line that starts with start. It reads the pipe
        itself, keeping what's past a line for the next call, since a
        buffered readline would hide lines already read from select."""
        buffered = getattr(client, "gesture_lines", b"")
        while True:
            while b"\n" not in buffered:
                self.assertTrue(select.select([client.stdout], [], [], TIMEOUT)[0],
                                f"no {start!r} from the window")
                chunk = os.read(client.stdout.fileno(), 4096)
                self.assertTrue(chunk, f"the window went before {start!r}")
                buffered += chunk
            line, buffered = buffered.split(b"\n", 1)
            client.gesture_lines = buffered
            if line.startswith(start):
                return line.decode().strip()

    def open(self, ipc, app_id):
        """A window that says which gestures it gets."""
        client = self.wm.open_window(app_id, app_id, stdout=True, gestures=True)
        self.addCleanup(client.stdout.close)
        ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                       and e["container"]["app_id"] == app_id)
        return client

    def point_at(self, app_id):
        """Moves the tests' pointer, a virtual one, into app_id's window."""
        output, = self.connect().request(GET_OUTPUTS)
        size = [str(output["rect"]["width"]), str(output["rect"]["height"])]
        rect, = [w["rect"] for w in self.windows().values() if w["app_id"] == app_id]
        subprocess.run([POINTER, "move", str(rect["x"] + 50), str(rect["y"] + 50), *size],
                       env=self.wm.env, check=True, timeout=TIMEOUT)

    def one_window(self):
        ipc = self.connect()
        ipc.subscribe("window")
        client = self.open(ipc, "test.app")
        # The headless seat has a pointer once a virtual one comes, and
        # keeps it; the window takes its own, and its gestures, then
        self.point_at("test.app")
        self.line(client, b"pointing")
        return client

    def test_a_pinch_reaches_the_window_under_the_pointer(self):
        client = self.one_window()
        self.command(self.connect(), "gesture pinch 2")
        self.assertEqual(self.line(client, b"pinch "), "pinch begin 2")
        self.assertEqual(self.line(client, b"pinch "), "pinch update 10.00 0.00 2.00 90.00")
        self.assertEqual(self.line(client, b"pinch "), "pinch end")

    def test_a_swipe_reaches_it(self):
        client = self.one_window()
        self.command(self.connect(), "gesture swipe 3")
        self.assertEqual(self.line(client, b"swipe "), "swipe begin 3")
        self.assertEqual(self.line(client, b"swipe "), "swipe update 10.00 0.00")
        self.assertEqual(self.line(client, b"swipe "), "swipe end")

    def test_a_hold_reaches_it(self):
        client = self.one_window()
        self.command(self.connect(), "gesture hold 1")
        self.assertEqual(self.line(client, b"hold "), "hold begin 1")
        self.assertEqual(self.line(client, b"hold "), "hold end")

    def test_only_the_window_under_the_pointer_gets_it(self):
        ipc = self.connect()
        ipc.subscribe("window")
        first = self.open(ipc, "test.first")
        second = self.open(ipc, "test.second")
        self.point_at("test.first")
        self.line(first, b"pointing")
        self.line(second, b"pointing")
        self.command(self.connect(), "gesture pinch 2")
        self.assertEqual(self.line(first, b"pinch begin"), "pinch begin 2")
        self.point_at("test.second")
        self.command(self.connect(), "gesture pinch 3")
        # The second window's first pinch is the one over it
        self.assertEqual(self.line(second, b"pinch begin"), "pinch begin 3")

    def test_a_bad_gesture_fails(self):
        for command in ["gesture", "gesture pinch", "gesture twirl 2", "gesture pinch 0",
                        "gesture pinch 2 more"]:
            result, = self.connect().request(RUN_COMMAND, command)
            self.assertFalse(result["success"], f"{command!r}: {result}")


if __name__ == "__main__":
    unittest.main()
