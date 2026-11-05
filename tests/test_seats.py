"""GET_SEATS, as sway answers it, with tidewm's own last_input: your last
key press or click, the window it went to and how long ago, for tide's
polkit agent, whose prompt takes focus only right after your input went
to the window asking (tide's SPEC §14.1)."""

import os
import select
import shutil
import subprocess
import unittest

from harness import (EVENT_WINDOW, EVENT_WORKSPACE, GET_OUTPUTS, GET_SEATS, ROOT, TIMEOUT,
                     TidewmTest)

POINTER = os.path.join(ROOT, "tests", "pointer")
LAUNCHER = os.path.join(ROOT, "tests", "launcher")


class SeatHelpers(TidewmTest):
    """Helpers, with no tests of their own."""

    def setUp(self):
        super().setUp()
        self.ipc = self.connect()
        self.ipc.subscribe("window")

    def open(self, app_id):
        self.wm.open_window(app_id, app_id)
        return self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                                   and e["container"]["app_id"] == app_id)["container"]["id"]

    def seat(self):
        seats = self.connect().request(GET_SEATS)
        self.assertEqual(len(seats), 1)
        return seats[0]

    def last_input(self):
        last = self.seat()["last_input"]
        if last:
            self.assertLess(last.pop("msec_ago"), TIMEOUT * 1000)
        return last

    def pointer(self, *actions):
        subprocess.run([POINTER, *actions], env=self.wm.env, check=True, timeout=TIMEOUT)

    def over(self, con_id):
        """The move action to a point over con_id, on the one output:
        inside even the 100x100 buffer the test client draws until it has
        drawn at the size tidewm gave it."""
        output, = self.connect().request(GET_OUTPUTS)
        rect = self.windows()[con_id]["rect"]
        return ("move", str(rect["x"] + 50), str(rect["y"] + 50),
                str(output["rect"]["width"]), str(output["rect"]["height"]))


class SeatTest(SeatHelpers):
    def test_the_seat_is_seat0_with_its_focus_and_devices(self):
        window = self.open("test.app")
        seat = self.seat()
        self.assertEqual((seat["name"], seat["focus"]), ("seat0", window))
        self.assertIsInstance(seat["devices"], list)
        self.assertIsNone(seat["last_input"])

    def test_a_prompt_may_take_the_keyboard_right_after_a_key_press(self):
        self.open("test.app")
        self.assertFalse(self.seat()["prompt_may_focus"])
        self.press("x")
        self.assertTrue(self.seat()["prompt_may_focus"])

    def test_a_click_alone_lets_no_prompt_take_the_keyboard(self):
        self.open("test.app")
        self.pointer("click")
        self.assertFalse(self.seat()["prompt_may_focus"])

    def test_a_key_press_went_to_the_focused_window(self):
        window = self.open("test.app")
        self.press("x")
        self.assertEqual(self.last_input(), {"type": "key", "window": window})

    def test_a_click_went_to_the_window_under_the_pointer(self):
        window = self.open("test.app")  # the only window, under the pointer
        self.pointer("click")
        self.assertEqual(self.last_input(), {"type": "button", "window": window})

    def test_a_second_button_went_where_the_held_one_did(self):
        left = self.open("test.left")
        right = self.open("test.right")
        # Held over left, the pointer keeps it as it moves over right
        self.pointer(*self.over(left), "press", "left", *self.over(right), "press", "right",
                     "release", "right", "release", "left")
        self.assertEqual(self.last_input(), {"type": "button", "window": left})

    def test_a_key_press_in_a_launcher_went_to_no_window(self):
        self.open("test.app")
        launcher = subprocess.Popen([LAUNCHER], env=self.wm.env, stdin=subprocess.PIPE,
                                    stdout=subprocess.PIPE)
        self.wm.clients.append(launcher)
        self.assertTrue(select.select([launcher.stdout], [], [], TIMEOUT)[0])
        self.assertEqual(launcher.stdout.readline(), b"open\n")
        self.press("x")
        self.assertEqual(self.last_input(), {"type": "key", "window": None})



class BindingTest(SeatHelpers):
    CONFIG = "bindsym x nop\nbindsym Mod1+x persist workspace held\n"

    def test_a_key_a_binding_took_went_to_no_window(self):
        self.open("test.app")
        self.press("x")
        self.assertEqual(self.last_input(), {"type": "key", "window": None})

    def test_a_click_a_binding_took_went_to_no_window(self):
        self.need(shutil.which("wtype"), "typing keys needs wtype")
        window = self.open("test.app")  # the only window, under the pointer
        workspaces = self.connect()
        workspaces.subscribe("workspace")
        # wtype holds Alt down while it waits on its input; its binding
        # says when it has
        typist = subprocess.Popen(["wtype", "-M", "alt", "-k", "x", "-"], env=self.wm.env,
                                  stdin=subprocess.PIPE)
        self.wm.clients.append(typist)
        workspaces.next_event(EVENT_WORKSPACE, lambda e: e["change"] == "init"
                            and e["current"]["name"] == "held")
        self.pointer(*self.over(window), "press", "left")  # Alt+left moves the window
        self.assertEqual(self.last_input(), {"type": "button", "window": None})
        typist.stdin.close()
        typist.wait(TIMEOUT)
        self.pointer("release", "left")


if __name__ == "__main__":
    unittest.main()
