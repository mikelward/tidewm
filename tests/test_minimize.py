"""Minimized windows (tide's SPEC §6.7): tidewm's own minimize command
hides a window, which leaves the layout, focus going to the window you
were in before. It stays on its workspace, which stays while it waits,
and focusing it, by any means, brings it back, at the end of its part's
stack."""

import unittest

from harness import EVENT_WINDOW, GET_OUTPUTS, GET_WORKSPACES, RUN_COMMAND, TidewmTest


class MinimizeTest(TidewmTest):
    def setUp(self):
        super().setUp()
        self.ipc = self.connect()
        self.ipc.subscribe("window")

    def open(self, app_id):
        _, con = self.open_window(self.ipc, app_id)
        return con["id"]

    def changed(self, con_id, minimized):
        """The next minimize event, which must be con_id's, minimized or
        brought back as given; its container."""
        event = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "minimize")
        self.assertEqual((event["container"]["id"], event["container"]["minimized"]),
                         (con_id, minimized))
        return event["container"]

    def minimize(self, con_id, how=""):
        self.command(self.connect(), f"[con_id={con_id}] minimize {how}".strip())

    def focused(self):
        ids = [i for i, w in self.windows().items() if w["focused"]]
        return ids[0] if ids else None

    def tiled(self):
        """The current workspace's tiled windows, in stack order."""
        return [i for i, w in self.windows().items()
                if w["type"] == "con" and w["workspace"] == self.current() and not w["minimized"]]

    def current(self):
        ws, = [w for w in self.connect().request(GET_WORKSPACES) if w["focused"]]
        return ws["name"]

    def fails(self, command):
        result, = self.connect().request(RUN_COMMAND, command)
        self.assertFalse(result["success"], f"{command!r}: {result}")
        return result

    def test_it_leaves_the_layout_and_focus_goes_to_the_window_before(self):
        a = self.open("test.a")
        alone = self.windows()[a]["rect"]
        b = self.open("test.b")
        self.assertNotEqual(self.windows()[a]["rect"], alone)
        self.minimize(b)
        con = self.changed(b, True)
        self.assertEqual((con["visible"], con["focused"]), (False, False))
        self.assertEqual(self.focused(), a)
        self.assertEqual(self.windows()[a]["rect"], alone, "a has the monitor to itself")
        self.assertTrue(self.windows()[a]["visible"])

    def test_focusing_it_brings_it_back_at_the_end_of_the_stack(self):
        a = self.open("test.a")
        b = self.open("test.b")
        c = self.open("test.c")
        self.minimize(a)
        self.changed(a, True)
        self.assertEqual(self.tiled(), [b, c])
        self.command(self.connect(), f"[con_id={a}] focus")
        con = self.changed(a, False)
        self.assertTrue(con["visible"])
        self.assertEqual(self.focused(), a)
        self.assertEqual(self.tiled(), [b, c, a])

    def test_minimize_disable_brings_it_back_without_focus(self):
        a = self.open("test.a")
        b = self.open("test.b")
        self.minimize(b)
        self.changed(b, True)
        self.minimize(b, "disable")
        self.changed(b, False)
        self.assertEqual(self.focused(), a)
        self.assertTrue(self.windows()[b]["visible"])

    def test_focus_next_passes_over_it(self):
        a = self.open("test.a")
        b = self.open("test.b")
        c = self.open("test.c")
        self.minimize(b)
        self.changed(b, True)
        self.command(self.connect(), f"[con_id={a}] focus")
        self.command(self.connect(), "focus next")
        self.assertEqual(self.focused(), c)
        self.command(self.connect(), "focus next")
        self.assertEqual(self.focused(), a)
        self.assertTrue(self.windows()[b]["minimized"])

    def test_the_last_window_minimized_leaves_nothing_focused(self):
        a = self.open("test.a")
        self.minimize(a)
        self.changed(a, True)
        self.assertIsNone(self.focused())

    def test_its_workspace_stays_while_it_waits_and_focusing_it_goes_there(self):
        self.command(self.connect(), "workspace two")
        a = self.open("test.a")
        self.minimize(a)
        self.changed(a, True)
        self.command(self.connect(), "workspace 1")
        self.assertIn("two", self.workspace_names())
        self.assertEqual(self.windows()[a]["workspace"], "two")
        self.command(self.connect(), f"[con_id={a}] focus")
        self.changed(a, False)
        self.assertEqual((self.current(), self.focused()), ("two", a))

    def test_it_keeps_its_states_while_it_waits(self):
        a = self.open("test.a")
        self.command(self.connect(), f"[con_id={a}] maximize enable")
        self.minimize(a)
        self.changed(a, True)
        self.open("test.b")
        self.assertTrue(self.windows()[a]["maximized"], "another window's focus doesn't end it")
        self.command(self.connect(), f"[con_id={a}] focus")
        self.changed(a, False)
        self.assertTrue(self.windows()[a]["maximized"])

    def test_a_fullscreen_one_stays_so_while_another_opens(self):
        a = self.open("test.a")
        self.command(self.connect(), f"[con_id={a}] fullscreen enable")
        self.minimize(a)
        self.changed(a, True)
        self.open("test.b")
        self.assertEqual(self.windows()[a]["fullscreen_mode"], 1, "a new window doesn't end it")
        self.command(self.connect(), f"[con_id={a}] focus")
        self.changed(a, False)
        self.assertEqual(self.windows()[a]["fullscreen_mode"], 1)

    def test_a_fullscreen_one_covers_its_output_as_it_is_when_it_comes_back(self):
        a = self.open("test.a")
        self.command(self.connect(), f"[con_id={a}] fullscreen enable")
        self.minimize(a)
        self.changed(a, True)
        self.command(self.connect(), "output HEADLESS-1 transform 90")
        output, = self.connect().request(GET_OUTPUTS)
        self.command(self.connect(), f"[con_id={a}] focus")
        self.changed(a, False)
        self.assertEqual(self.windows()[a]["rect"], output["rect"], "the rotated output")

    def test_the_command_takes_enable_disable_or_toggle(self):
        a = self.open("test.a")
        self.assertTrue(self.fails("minimize maybe")["parse_error"])
        self.minimize(a, "toggle")
        self.changed(a, True)
        self.minimize(a, "toggle")
        self.changed(a, False)
        self.command(self.connect(), "workspace two")
        self.assertIn("No window to minimize", self.fails("minimize")["error"])


if __name__ == "__main__":
    unittest.main()
