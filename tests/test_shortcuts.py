"""send_shortcut, tidewm's own, as Hyprland's sendshortcut: a shortcut
sent to a window, focused or not, as tide's picker pastes (docs/ipc.md).

The headless backend has no keyboard, so a virtual one, the tests', is
the seat's, on its second layout, German, where Z and Y swap places: the
key sent is the one that layout gives the name."""

import os
import select
import subprocess
import unittest

from harness import EVENT_WINDOW, ROOT, RUN_COMMAND, TIMEOUT, TidewmTest

KEYBOARD = os.path.join(ROOT, "tests", "keyboard")


class ShortcutTest(TidewmTest):
    def setUp(self):
        super().setUp()
        keyboard = subprocess.Popen([KEYBOARD, "1"], env=dict(self.wm.env, XKB_LAYOUT="us,de"),
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.wm.clients.append(keyboard)
        self.addCleanup(keyboard.stdout.close)
        self.addCleanup(keyboard.stdin.close)
        self.assertEqual(self.line(keyboard), b"switched\n")

    def line(self, client):
        self.assertTrue(select.select([client.stdout], [], [], TIMEOUT)[0], "nothing came")
        return client.stdout.readline()

    def typist(self, ipc, app_id):
        """A window that says which keys it gets, and its container."""
        client = self.wm.open_window(app_id, app_id, stdout=True, keys=True)
        self.addCleanup(client.stdout.close)
        con = ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                             and e["container"]["app_id"] == app_id)["container"]
        self.assertEqual(self.line(client), b"typing\n")
        return client, con

    def fails(self, command):
        result, = self.connect().request(RUN_COMMAND, command)
        self.assertFalse(result["success"], f"{command!r}: {result}")
        return result["error"]

    def test_a_shortcut_goes_to_the_window_named_and_focus_stays(self):
        ipc = self.connect()
        ipc.subscribe("window")
        first, first_con = self.typist(ipc, "test.first")
        second, second_con = self.typist(ipc, "test.second")
        self.command(self.connect(), f"[con_id={first_con['id']}] send_shortcut ctrl+z")
        self.assertEqual(self.line(first), b"key ctrl+z\n")
        self.assertEqual([i for i, w in self.windows().items() if w["focused"]],
                         [second_con["id"]])
        # The focused window got nothing until its own, without criteria
        self.command(self.connect(), "send_shortcut Ctrl+Shift+V")
        self.assertEqual(self.line(second), b"key ctrl+shift+V\n")
        # A name on a key's shifted level, with shift given: German's
        # asterisk is shift and its plus key
        self.command(self.connect(), "send_shortcut ctrl+shift+asterisk")
        self.assertEqual(self.line(second), b"key ctrl+shift+asterisk\n")

    def test_send_shortcut_errors(self):
        ipc = self.connect()
        ipc.subscribe("window")
        self.typist(ipc, "test.app")
        for command, error in [
            ("send_shortcut", "Expected send_shortcut MODS+KEY, such as ctrl+v"),
            ("send_shortcut ctrl+ v", "Expected send_shortcut MODS+KEY, such as ctrl+v"),
            ("send_shortcut hyper+v", "send_shortcut's modifiers are ctrl, shift, alt and super"),
            ("send_shortcut ctrl+nokey", "send_shortcut's key isn't a key name"),
            ("send_shortcut ctrl+", "send_shortcut's key isn't a key name"),
        ]:
            self.assertIn(error, self.fails(command), command)
        self.assertEqual(self.fails("[con_id=999999] send_shortcut ctrl+v"),
                         "No window matches the criteria")


if __name__ == "__main__":
    unittest.main()
