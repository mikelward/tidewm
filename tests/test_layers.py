"""Layer surfaces and the keyboard, as tide's SPEC §14.1 has it: one with
on-demand focus, such as a notification's reply field, takes the keyboard
when you click it, not when the pointer crosses it, and keeps it until you
move it, by a click elsewhere, a key binding or a command. A window
opening or closing, or the pointer crossing one, doesn't take it."""

import os
import select
import subprocess
import unittest

from harness import (EVENT_WINDOW, GET_OUTPUTS, ROOT, TIMEOUT, TidewmTest)

POINTER = os.path.join(ROOT, "tests", "pointer")
LAUNCHER = os.path.join(ROOT, "tests", "launcher")


class OnDemandTest(TidewmTest):
    CONFIG = "bindsym Mod1+x focus next\n"

    def setUp(self):
        super().setUp()
        # The headless seat has a keyboard once a virtual one comes, and
        # keeps it; the notification takes its own then
        self.press("-k", "Escape")
        self.ipc = self.connect()
        self.ipc.subscribe("window")

    def open(self, app_id):
        """Opens a window, returning its container and its client."""
        client = self.wm.open_window(app_id, app_id)
        window = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                                     and e["container"]["app_id"] == app_id)["container"]
        return window, client

    def notification(self):
        """Opens tests/launcher's notification, in the top right corner,
        which takes the keyboard when it's clicked."""
        note = subprocess.Popen([LAUNCHER, "notification"], env=self.wm.env,
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.wm.clients.append(note)
        self.addCleanup(note.stdout.close)
        self.assertEqual(self.line(note), b"open\n")
        return note

    def line(self, process):
        self.assertTrue(select.select([process.stdout], [], [], TIMEOUT)[0], "it said nothing")
        return process.stdout.readline()

    def point(self, how, x, y, *actions):
        """Moves the pointer to x, y, how tests/pointer does ("move" or
        "cross"), then does the actions."""
        output, = self.connect().request(GET_OUTPUTS)
        size = [str(output["rect"]["width"]), str(output["rect"]["height"])]
        subprocess.run([POINTER, how, str(x), str(y), *size, *actions], env=self.wm.env,
                       check=True, timeout=TIMEOUT)

    def at_notification(self, how, *actions):
        output, = self.connect().request(GET_OUTPUTS)
        self.point(how, output["rect"]["width"] - 100, 50, *actions)

    def over(self, window, how, *actions):
        """Moves the pointer to the middle of window, clear of the
        notification, then does the actions."""
        rect = window["rect"]
        self.point(how, rect["x"] + rect["width"] // 2, rect["y"] + rect["height"] // 2, *actions)

    def typing_in(self, note):
        """Clicks the notification's reply field, which takes the keyboard."""
        self.at_notification("move", "click")
        self.assertEqual(self.line(note), b"focused\n")
        self.assertEqual(self.focused(), [])

    def typed(self, note):
        """Types a key, and asserts the notification has it, as it would
        have lost the keyboard first otherwise."""
        self.press("-k", "a")
        self.assertEqual(self.line(note), b"key\n")

    def focused(self):
        return [i for i, w in self.windows().items() if w["focused"]]

    def test_the_pointer_crossing_a_notification_leaves_the_window_the_keyboard(self):
        window, _ = self.open("test.first")
        self.notification()
        self.over(window, "cross")
        self.at_notification("cross")
        self.assertEqual(self.focused(), [window["id"]])

    def test_a_click_gives_a_notification_the_keyboard(self):
        self.open("test.first")
        note = self.notification()
        self.typing_in(note)
        self.typed(note)

    def test_a_window_opening_doesnt_take_it(self):
        self.open("test.first")
        note = self.notification()
        self.typing_in(note)
        self.open("test.second")
        self.assertEqual(self.focused(), [])
        self.typed(note)

    def test_the_pointer_crossing_a_window_doesnt_take_it(self):
        window, _ = self.open("test.first")
        note = self.notification()
        self.typing_in(note)
        self.over(window, "cross")
        self.assertEqual(self.focused(), [])
        self.typed(note)

    def test_the_last_window_closing_doesnt_take_it(self):
        _, client = self.open("test.first")
        note = self.notification()
        self.typing_in(note)
        client.kill()
        client.wait(TIMEOUT)
        self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "close")
        self.typed(note)

    def test_a_click_on_a_window_takes_it(self):
        window, _ = self.open("test.first")
        note = self.notification()
        self.typing_in(note)
        self.over(window, "move", "click")
        self.assertEqual(self.line(note), b"unfocused\n")
        self.assertEqual(self.focused(), [window["id"]])

    def test_a_command_takes_it(self):
        window, _ = self.open("test.first")
        note = self.notification()
        self.typing_in(note)
        self.command(self.connect(), f"[con_id={window['id']}] focus")
        self.assertEqual(self.line(note), b"unfocused\n")
        self.assertEqual(self.focused(), [window["id"]])

    def test_a_key_binding_takes_it(self):
        window, _ = self.open("test.first")
        note = self.notification()
        self.typing_in(note)
        self.press("-M", "alt", "-k", "x", "-m", "alt")
        self.assertEqual(self.line(note), b"unfocused\n")
        self.assertEqual(self.focused(), [window["id"]])

    def test_the_launcher_takes_it(self):
        self.open("test.first")
        note = self.notification()
        self.typing_in(note)
        launcher = subprocess.Popen([LAUNCHER], env=self.wm.env, stdin=subprocess.PIPE,
                                    stdout=subprocess.PIPE)
        self.wm.clients.append(launcher)
        self.addCleanup(launcher.stdout.close)
        self.assertEqual(self.line(launcher), b"open\n")
        self.assertEqual(self.line(note), b"unfocused\n")

    def test_the_lock_takes_it(self):
        self.open("test.first")
        note = self.notification()
        self.typing_in(note)
        self.lock()
        self.assertEqual(self.line(note), b"unfocused\n")


if __name__ == "__main__":
    unittest.main()
