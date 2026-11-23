"""zoom next and prev, tide's Alt+Tab (its SPEC §6.6): a tap makes the
focused window the master; held, each further Tab moves focus to the
next window, Shift going back, and letting go of Alt makes that one the
master; Escape puts focus back."""

import os
import select
import subprocess
import unittest

from harness import EVENT_WINDOW, GET_WORKSPACES, ROOT, RUN_COMMAND, TIMEOUT, TidewmTest

KEYS = "bindsym Mod1+Tab zoom next\nbindsym Mod1+Shift+Tab zoom prev\n"
KEYBOARD = os.path.join(ROOT, "tests", "keyboard")


class ZoomCycleTest(TidewmTest):
    CONFIG = KEYS

    def setUp(self):
        super().setUp()
        self.ipc = self.connect()
        self.ipc.subscribe("window")
        # The first the master, the rest its stack, the last focused
        self.first = self.open_window(self.ipc, "test.first")[1]
        self.second = self.open_window(self.ipc, "test.second")[1]
        self.third = self.open_window(self.ipc, "test.third")[1]
        self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "focus"
                            and e["container"]["id"] == self.third["id"])

    def stack(self):
        """The windows' app IDs in tile's order: the master, then the stack."""
        windows = self.windows()
        cons = [windows[c["id"]] for c in (self.first, self.second, self.third)]
        return [c["app_id"] for c in sorted(cons, key=lambda c: (c["rect"]["x"], c["rect"]["y"]))]

    def focused(self):
        return [w["app_id"] for w in self.windows().values() if w["focused"]]

    def alt(self, *keys):
        """Presses each of keys, "Tab", "shift+Tab" or "Escape", with Alt
        held throughout, then lets go of Alt."""
        args = ["-M", "alt"]
        for key in keys:
            if key.startswith("shift+"):
                args += ["-M", "shift", "-k", key[6:], "-m", "shift"]
            else:
                args += ["-k", key]
        self.press(*args, "-m", "alt")

    def keyboard(self, keys):
        """A keyboard of its own sends keys, evdev codes as tests/keyboard
        takes them, and stays, with any it left down held, until the test
        is done"""
        keyboard = subprocess.Popen([KEYBOARD, "0"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    env=dict(self.wm.env, XKB_LAYOUT="us", KEYS=keys))
        self.addCleanup(keyboard.wait)
        self.addCleanup(keyboard.stdin.close)
        self.addCleanup(keyboard.stdout.close)
        self.assertTrue(select.select([keyboard.stdout], [], [], TIMEOUT)[0], "tests/keyboard said nothing")
        self.assertEqual(keyboard.stdout.readline(), b"switched\n")
        return keyboard

    def test_a_tap_makes_the_focused_window_the_master(self):
        self.alt("Tab")
        self.assertEqual(self.stack(), ["test.third", "test.first", "test.second"])
        self.assertEqual(self.focused(), ["test.third"])

    def test_tab_held_moves_focus_on_and_letting_go_makes_it_the_master(self):
        # From the last, round to the master, then on to the second
        self.alt("Tab", "Tab", "Tab")
        self.assertEqual(self.stack(), ["test.second", "test.first", "test.third"])
        self.assertEqual(self.focused(), ["test.second"])

    def test_shift_goes_back(self):
        self.alt("Tab", "shift+Tab")
        self.assertEqual(self.stack(), ["test.second", "test.first", "test.third"])

    def test_letting_go_on_the_master_leaves_it(self):
        self.alt("Tab", "Tab")
        self.assertEqual(self.stack(), ["test.first", "test.second", "test.third"])
        self.assertEqual(self.focused(), ["test.first"])

    def test_escape_puts_focus_back_and_makes_no_master(self):
        self.alt("Tab", "Tab", "Tab", "Escape")
        self.assertEqual(self.stack(), ["test.first", "test.second", "test.third"])
        self.assertEqual(self.focused(), ["test.third"])

    def test_another_keyboard_leaves_it_under_way(self):
        # Alt (56) held on one keyboard, Tab (15) three times, round to
        # the second; Shift on another is no letting go of Alt
        self.keyboard("56+ 15+ 15- 15+ 15- 15+ 15-")
        self.press("-M", "shift", "-m", "shift")
        self.assertEqual(self.stack(), ["test.first", "test.second", "test.third"])
        self.assertEqual(self.focused(), ["test.second"])

    def test_the_keyboard_going_ends_it_with_no_master(self):
        keyboard = self.keyboard("56+ 15+ 15- 15+ 15- 15+ 15-")
        keyboard.stdin.close()
        keyboard.wait()
        # Alt+Tab on this keyboard starts afresh: a tap, the second the master
        self.alt("Tab")
        self.assertEqual(self.stack(), ["test.second", "test.first", "test.third"])

    def test_escape_on_another_workspace_leaves_focus_there(self):
        # Alt held and Tab on one keyboard, another workspace, then
        # Escape: the window it began in is hidden there
        self.keyboard("56+ 15+ 15-")
        self.command(self.connect(), "workspace 2")
        self.press("-k", "Escape")
        self.assertEqual(self.focused(), [])
        current, = [w["name"] for w in self.connect().request(GET_WORKSPACES) if w["focused"]]
        self.assertEqual(current, "2")

    def test_over_the_ipc_while_alt_is_held_it_is_zoom(self):
        # Alt held on a keyboard of its own, Tab once: under way
        self.keyboard("56+ 15+ 15-")
        self.command(self.connect(), "zoom next")
        self.assertEqual(self.stack(), ["test.third", "test.first", "test.second"])
        self.assertEqual(self.focused(), ["test.third"])

    def test_with_no_keys_held_it_is_zoom(self):
        self.command(self.connect(), "zoom next")
        self.assertEqual(self.stack(), ["test.third", "test.first", "test.second"])

    def test_it_takes_only_next_or_prev(self):
        result, = self.connect().request(RUN_COMMAND, "zoom sideways")
        self.assertFalse(result["success"])


if __name__ == "__main__":
    unittest.main()
