"""X11 windows in their outputs' own pixels, with xwayland_scaling disable,
as tide's SPEC §17 has them, so they're sharp at a fractional scale; and
scaled, by default, as in sway. The output is 800x600 at 1.25, so 640x480
in the layout, and the window is fullscreen on it, with no border."""

import os
import select
import subprocess
import unittest

from harness import (EVENT_WINDOW, GET_CONFIG, GET_OUTPUTS, ROOT, RUN_COMMAND, TIMEOUT,
                     TidewmTest)

POINTER = os.path.join(ROOT, "tests", "pointer")
OUTPUT = "output HEADLESS-1 mode 800x600@50Hz scale 1.25\n"


class X11Helpers(TidewmTest):
    """Helpers, with no tests of their own."""
    CONFIG = OUTPUT

    def setUp(self):
        super().setUp()
        self.need(self.wm.x11, "X11 windows need tidewm built with XWAYLAND")
        self.ipc = self.connect()
        self.ipc.subscribe("window")
        # The headless seat has a pointer once a virtual one comes, and
        # keeps it: Xwayland, which starts for the first X client, takes it
        # as it starts, before any window can hear of it
        self.pointer(("move", "0", "0"))

    def open(self):
        """Opens a fullscreen X11 window that says its size and clicks,
        returning its client and what it says of the X screen."""
        client = self.wm.open_x11_window("test.x11", "X11", fullscreen=True, report=True)
        screen = self.line(client)
        self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new")
        return client, screen

    def line(self, client):
        self.assertTrue(select.select([client.stdout], [], [], TIMEOUT)[0], "it said nothing")
        return client.stdout.readline()

    def sized(self, client, width, height):
        """Waits for the window to say it's width by height, in X11's pixels."""
        want = f"size {width} {height}\n".encode()
        while self.line(client) != want:
            pass

    def heard(self, client, start):
        """The window's next line that starts with start."""
        line = self.line(client)
        while not line.startswith(start):
            line = self.line(client)
        return line

    def pointer(self, *actions):
        """Does tests/pointer's actions, moves in the layout."""
        rects = [o["rect"] for o in self.connect().request(GET_OUTPUTS)]
        size = [str(max(r["x"] + r["width"] for r in rects)),
                str(max(r["y"] + r["height"] for r in rects))]
        args = []
        for action in actions:
            args += action
            if action[0] == "move":
                args += size
        subprocess.run([POINTER, *args], env=self.wm.env, check=True, timeout=TIMEOUT)

    def opened(self, client):
        """The X11 window client opened, once it's said what the screen is."""
        self.line(client)
        return self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new")["container"]

    def clicked(self, client, x, y):
        """Clicks at x, y in the layout, and returns where the window heard it.
        Within its first 100x100 pixels, so before Xwayland's buffer at its
        new size comes too."""
        self.pointer(("move", str(x), str(y)), ("click",))
        return self.heard(client, b"button ")


class UnscaledTest(X11Helpers):
    CONFIG = "xwayland_scaling disable\n" + OUTPUT

    def test_an_x11_window_is_its_outputs_pixels(self):
        client, _ = self.open()
        self.sized(client, 800, 600)

    def test_the_x11_screen_is_the_outputs_pixels(self):
        _, screen = self.open()
        self.assertEqual(screen, b"screen 800 600\n")

    def test_a_click_reaches_an_x11_window_in_its_pixels(self):
        client, _ = self.open()
        self.sized(client, 800, 600)
        self.assertEqual(self.clicked(client, 20, 16), b"button 25 20\n")

    def test_a_drag_out_of_an_x11_window_stays_in_its_pixels(self):
        client = self.wm.open_x11_window("test.x11", "X11", report=True)
        self.line(client)  # the screen
        x11 = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new")["container"]
        # Beside it, so it's the left tile, 352 wide, in a 1px border
        self.wm.open_window("test.other", "test.other")
        self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new")
        rect = self.windows()[x11["id"]]["rect"]
        x, y = rect["x"] + 1, rect["y"] + 1
        # Pressed within its first 100x100 pixels, as clicked() is
        self.pointer(("move", str(x + 20), str(y + 16)), ("press", "left"),
                     ("move", str(x + 400), str(y + 40)), ("release", "left"))
        self.assertEqual(self.heard(client, b"button "), b"button 25 20\n")
        # Over the other window now, but still the X11 window's drag
        self.assertEqual(self.heard(client, b"motion "), b"motion 500 50\n")

    def test_scaling_again_scales_an_x11_window_at_once(self):
        client, _ = self.open()
        self.sized(client, 800, 600)
        self.wm.write_config(OUTPUT)
        result, = self.connect().request(RUN_COMMAND, "reload")
        self.assertTrue(result["success"], result)
        self.sized(client, 640, 480)

    def test_xwayland_scaling_takes_enable_or_disable(self):
        self.wm.write_config("xwayland_scaling off\n" + OUTPUT)
        result, = self.connect().request(RUN_COMMAND, "reload")
        self.assertFalse(result["success"], result)
        self.assertIn("xwayland_scaling takes enable or disable", result["error"])
        config = self.connect().request(GET_CONFIG)
        self.assertEqual(config["config"], self.CONFIG)


class TwoOutputTest(X11Helpers):
    """A monitor at 1 beside one at 2: 800x600 and 400x300 in the layout,
    and in X11's space, spread by 2, 800x600 from 0 and from 1600."""
    OUTPUTS = 2
    CONFIG = ("xwayland_scaling disable\n"
              "output HEADLESS-1 mode 800x600@50Hz scale 1 position 0 0\n"
              "output HEADLESS-2 mode 800x600@50Hz scale 2 position 800 0\n")

    def on_the_left(self, window):
        """Moves window to the monitor at 1, if it isn't there."""
        self.command(self.connect(), f"[con_id={window['id']}] move to output HEADLESS-1")
        self.assertEqual(self.windows()[window["id"]]["output"], "HEADLESS-1")

    def test_a_fixed_size_x11_window_keeps_its_pixels_on_another_monitor(self):
        client = self.wm.open_x11_window("test.x11", "X11", report=True, fixed_size="400x300")
        window = self.opened(client)
        self.on_the_left(window)
        self.sized(client, 400, 300)
        self.command(self.connect(), f"[con_id={window['id']}] move to output HEADLESS-2")
        # Its 400x300 pixels are 200x150 there, in a 1px border
        rect = self.windows()[window["id"]]["rect"]
        self.assertEqual((rect["width"], rect["height"]), (202, 152))

    def test_a_configure_request_is_in_the_pixels_of_the_windows_monitor(self):
        client = self.wm.open_x11_window("test.x11", "X11", stdin=True, report=True)
        window = self.opened(client)
        self.on_the_left(window)
        self.command(self.connect(), f"[con_id={window['id']}] floating enable")
        # A place on the monitor at 2, but the window's on the one at 1
        client.stdin.write(b"configure 1700 100 420 320\n")
        self.sized(client, 420, 320)


class ScaledTest(X11Helpers):
    def test_an_x11_window_is_scaled_by_default(self):
        client, screen = self.open()
        self.assertEqual(screen, b"screen 640 480\n")
        self.sized(client, 640, 480)

    def test_a_click_reaches_a_scaled_x11_window_in_the_layouts_units(self):
        client, _ = self.open()
        self.sized(client, 640, 480)
        self.assertEqual(self.clicked(client, 20, 16), b"button 20 16\n")


if __name__ == "__main__":
    unittest.main()
