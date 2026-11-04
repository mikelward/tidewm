"""Maximize (tide's SPEC §6.3): one window covers its monitor's window
area, the bar still showing, above the other tiles, which stay where they
are. It lasts until you toggle it, close the window, or another window on
its workspace takes focus. tidewm's own maximize command maximizes the
window, and so does the app's own request."""

import os
import select
import subprocess
import unittest

from harness import (EVENT_WINDOW, GET_OUTPUTS, GET_SEATS, GET_WORKSPACES, ROOT, RUN_COMMAND,
                     TIMEOUT, TidewmTest)

LAUNCHER = os.path.join(ROOT, "tests", "launcher")
POINTER = os.path.join(ROOT, "tests", "pointer")


class MaximizeHelpers(TidewmTest):
    """Helpers, with no tests of their own."""

    def setUp(self):
        super().setUp()
        # A bar, so the window area isn't the whole output
        bar = subprocess.Popen([LAUNCHER, "bar"], env=self.wm.env, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE)
        self.wm.clients.append(bar)
        self.assertTrue(select.select([bar.stdout], [], [], TIMEOUT)[0])
        self.assertEqual(bar.stdout.readline(), b"open\n")
        self.ipc = self.connect()
        self.ipc.subscribe("window")

    def open(self, app_id, **kwargs):
        client = self.wm.open_window(app_id, app_id, stdin=True, stdout=True, **kwargs)
        con = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                                  and e["container"]["app_id"] == app_id)["container"]
        return con["id"], client

    def send(self, client, line):
        client.stdin.write(line.encode() + b"\n")
        client.stdin.flush()

    def told(self, client, expected):
        """What the app heard next: "maximized" or "unmaximized"."""
        self.assertTrue(select.select([client.stdout], [], [], TIMEOUT)[0], f"not {expected}")
        self.assertEqual(client.stdout.readline().decode().strip(), expected)

    def changed(self, con_id, maximized):
        event = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "maximize"
                                    and e["container"]["id"] == con_id)
        self.assertEqual(event["container"]["maximized"], maximized)

    def maximize(self, con_id, how=""):
        self.command(self.connect(), f"[con_id={con_id}] maximize {how}".strip())

    def area(self):
        """The current workspace's window area: the output's, less the bar."""
        ws, = [w for w in self.connect().request(GET_WORKSPACES) if w["focused"]]
        return ws["rect"]

    def rect(self, con_id):
        return self.windows()[con_id]["rect"]

    def maximized(self, con_id):
        return self.windows()[con_id]["maximized"]

    def fails(self, command):
        result, = self.connect().request(RUN_COMMAND, command)
        self.assertFalse(result["success"], f"{command!r}: {result}")
        return result

    def clicked(self, con_id):
        """The window a click over con_id goes to: whichever is on top
        there."""
        output, = self.connect().request(GET_OUTPUTS)
        rect = self.rect(con_id)
        subprocess.run([POINTER, "move", str(rect["x"] + 50), str(rect["y"] + 50),
                        str(output["rect"]["width"]), str(output["rect"]["height"]), "click"],
                       env=self.wm.env, check=True, timeout=TIMEOUT)
        seat, = self.connect().request(GET_SEATS)
        return seat["last_input"]["window"]


class MaximizeTest(MaximizeHelpers):
    def test_it_covers_the_window_area_over_the_other_tiles(self):
        a, a_client = self.open("test.a")
        b, _ = self.open("test.b")
        self.command(self.connect(), f"[con_id={a}] focus")
        a_rect, b_rect = self.rect(a), self.rect(b)
        self.command(self.connect(), "maximize")
        self.changed(a, True)
        self.told(a_client, "maximized")
        output, = self.connect().request(GET_OUTPUTS)
        self.assertLess(self.area()["height"], output["rect"]["height"])  # the bar shows
        self.assertEqual((self.rect(a), self.rect(b)), (self.area(), b_rect))
        self.command(self.connect(), "maximize toggle")
        self.changed(a, False)
        self.told(a_client, "unmaximized")
        self.assertEqual(self.rect(a), a_rect)

    def test_another_window_on_the_workspace_taking_focus_ends_it(self):
        a, a_client = self.open("test.a")
        b, _ = self.open("test.b")
        self.command(self.connect(), f"[con_id={a}] focus")
        self.maximize(a)
        self.changed(a, True)
        self.command(self.connect(), f"[con_id={b}] focus")
        self.changed(a, False)
        self.told(a_client, "maximized")
        self.told(a_client, "unmaximized")
        self.assertNotEqual(self.rect(a), self.area())

    def test_a_new_window_taking_focus_ends_it(self):
        a, _ = self.open("test.a")
        self.maximize(a)
        self.changed(a, True)
        self.open("test.b")  # its "new" comes after a's "maximize"
        self.assertFalse(self.maximized(a))

    def test_focus_on_another_workspace_leaves_it(self):
        a, _ = self.open("test.a")
        self.maximize(a)
        self.changed(a, True)
        self.command(self.connect(), "workspace two")
        self.open("test.b")
        self.assertTrue(self.maximized(a))
        self.command(self.connect(), "workspace 1")
        self.assertEqual((self.maximized(a), self.rect(a)), (True, self.area()))

    def test_its_dialog_taking_focus_leaves_it(self):
        a, a_client = self.open("test.a")
        self.open("test.b")
        self.command(self.connect(), f"[con_id={a}] focus")
        self.maximize(a)
        self.changed(a, True)
        self.send(a_client, "dialog")
        dialog = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                                     and e["container"]["app_id"] == "test.a.dialog")
        windows = self.windows()
        self.assertTrue(windows[dialog["container"]["id"]]["focused"])
        self.assertTrue(windows[a]["maximized"])

    def test_it_stays_under_its_dialog_in_the_floating_layout(self):
        a, a_client = self.open("test.a")
        self.open("test.b")
        self.command(self.connect(), f"[con_id={a}] focus")
        self.maximize(a)
        self.changed(a, True)
        self.send(a_client, "dialog")
        dialog = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                                     and e["container"]["app_id"] == "test.a.dialog")
        dialog = dialog["container"]["id"]
        self.command(self.connect(), "layout floating")
        self.assertEqual(self.clicked(dialog), dialog)
        self.assertTrue(self.maximized(a))

    def test_the_app_asks_to_be_maximized_and_not(self):
        a, a_client = self.open("test.a")
        self.open("test.b")
        self.send(a_client, "maximize")
        self.changed(a, True)
        self.told(a_client, "maximized")
        self.assertEqual(self.rect(a), self.area())
        self.send(a_client, "unmaximize")
        self.changed(a, False)
        self.told(a_client, "unmaximized")
        self.assertNotEqual(self.rect(a), self.area())

    def test_an_app_that_asks_before_it_maps_opens_maximized(self):
        self.open("test.b")
        a, a_client = self.open("test.a", maximized=True)
        self.told(a_client, "maximized")
        self.assertEqual((self.maximized(a), self.rect(a)), (True, self.area()))

    def test_it_ends_when_the_window_closes(self):
        a, a_client = self.open("test.a")
        self.maximize(a)
        self.told(a_client, "maximized")
        # Unmapped and mapped again, it's a new window, not maximized
        self.send(a_client, "remap")
        self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                            and e["container"]["id"] == a)
        self.told(a_client, "unmaximized")
        self.assertFalse(self.maximized(a))

    def test_a_floating_window_gets_its_place_back(self):
        a, _ = self.open("test.a")
        self.open("test.b")
        self.command(self.connect(), f"[con_id={a}] floating enable")
        before = self.rect(a)
        self.maximize(a)
        self.changed(a, True)
        self.assertEqual(self.rect(a), self.area())
        self.maximize(a, "disable")
        self.changed(a, False)
        self.assertEqual(self.rect(a), before)

    def test_a_window_in_the_floating_layout_gets_its_place_back(self):
        a, _ = self.open("test.a")
        self.open("test.b")
        self.command(self.connect(), "layout floating")
        before = self.rect(a)
        self.maximize(a)
        self.changed(a, True)
        self.maximize(a, "disable")
        self.changed(a, False)
        self.assertEqual(self.rect(a), before)

    def test_a_window_in_another_workspaces_floating_layout_gets_its_place_back(self):
        a, _ = self.open("test.a")
        self.open("test.b")
        self.command(self.connect(), "layout floating")
        before = self.rect(a)
        self.maximize(a)
        self.changed(a, True)
        self.command(self.connect(), "workspace two")
        self.command(self.connect(), "layout tile")
        self.maximize(a, "disable")
        self.changed(a, False)
        self.command(self.connect(), "workspace 1")
        self.assertEqual(self.rect(a), before)

    def test_a_floating_window_unmaximized_while_fullscreen_gets_its_place_back(self):
        a, _ = self.open("test.a")
        self.open("test.b")
        self.command(self.connect(), f"[con_id={a}] floating enable")
        before = self.rect(a)
        self.maximize(a)
        self.command(self.connect(), f"[con_id={a}] fullscreen enable")
        self.maximize(a, "disable")
        self.command(self.connect(), f"[con_id={a}] fullscreen disable")
        self.assertEqual((self.maximized(a), self.rect(a)), (False, before))

    def test_after_fullscreen_it_is_maximized_again(self):
        a, _ = self.open("test.a")
        self.maximize(a)
        self.changed(a, True)
        self.command(self.connect(), f"[con_id={a}] fullscreen enable")
        output, = self.connect().request(GET_OUTPUTS)
        self.assertEqual(self.rect(a), output["rect"])
        self.command(self.connect(), f"[con_id={a}] fullscreen disable")
        self.assertEqual((self.maximized(a), self.rect(a)), (True, self.area()))

    def test_the_command_takes_enable_disable_or_toggle(self):
        self.open("test.a")
        self.assertTrue(self.fails("maximize maybe")["parse_error"])
        self.command(self.connect(), "workspace two")
        self.assertIn("No window to maximize", self.fails("maximize")["error"])



class FloatingTest(MaximizeHelpers):
    CONFIG = 'for_window [app_id="^test.float$"] floating enable\n'

    def test_a_floating_window_stays_under_its_dialog(self):
        a, a_client = self.open("test.float")
        self.maximize(a)
        self.changed(a, True)
        self.send(a_client, "dialog")
        dialog = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                                     and e["container"]["app_id"] == "test.float.dialog")
        dialog = dialog["container"]["id"]
        self.command(self.connect(), "mfact +0.05")  # arranged again
        self.assertEqual(self.clicked(dialog), dialog)
        self.assertTrue(self.maximized(a))

    def test_a_floating_window_that_opened_maximized_keeps_its_size(self):
        a, a_client = self.open("test.float", maximized=True)
        self.told(a_client, "maximized")
        self.assertEqual(self.windows()[a]["type"], "floating_con")
        area = self.area()
        self.assertEqual(self.rect(a), area)
        self.maximize(a, "disable")
        self.changed(a, False)
        self.assertEqual(self.rect(a), area)


if __name__ == "__main__":
    unittest.main()
