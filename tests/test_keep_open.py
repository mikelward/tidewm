"""keep_open, tidewm's own, for tide's pinned windows (its SPEC §6.5): a
window kept open stays when it's told to close, by kill or a key bound to
kill, and a close_refused event says so, for the shell to tell you why.
A binding with --kept-open counts only while the keyboard is in a window
kept open, so it can take an app's own close keys from that window alone.
It's kept open on the workspace it's on: on any other, however it got
there, it isn't, and minimized, it closes as any other (docs/ipc.md,
docs/config.md)."""

import os
import select
import subprocess
import unittest

from harness import EVENT_BINDING, EVENT_WINDOW, ROOT, RUN_COMMAND, TIMEOUT, TidewmTest

KEYBOARD = os.path.join(ROOT, "tests", "keyboard")


class KeepOpenHelpers(TidewmTest):
    def setUp(self):
        super().setUp()
        self.ipc = self.connect()
        self.ipc.subscribe("window")

    def line(self, client):
        self.assertTrue(select.select([client.stdout], [], [], TIMEOUT)[0], "nothing came")
        return client.stdout.readline()

    def open(self, app_id, keys=False):
        """A window that answers ping, and its container's id."""
        client = self.wm.open_window(app_id, app_id, stdin=True, stdout=True, keys=keys)
        self.addCleanup(client.stdout.close)
        con = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                                  and e["container"]["app_id"] == app_id)["container"]
        if keys:
            self.assertEqual(self.line(client), b"typing\n")
        return client, con["id"]

    def still_open(self, client):
        """Asserts the compositor hasn't asked client to close: it would
        have gone before it answered the ping."""
        client.stdin.write(b"ping\n")
        client.stdin.flush()
        self.assertEqual(self.line(client), b"pong\n")

    def keep(self, con_id, how="enable"):
        self.command(self.connect(), f"[con_id={con_id}] keep_open {how}")

    def fails(self, command):
        result, = self.connect().request(RUN_COMMAND, command)
        self.assertFalse(result["success"], f"{command!r}: {result}")
        return result["error"]

    def refused(self, con_id):
        """Asserts the next close_refused event is con_id's."""
        event = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "close_refused")
        self.assertEqual(event["container"]["id"], con_id)

    def closed(self, con_id):
        event = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "close")
        self.assertEqual(event["container"]["id"], con_id)


class KeepOpenTest(KeepOpenHelpers):
    def kept(self, con_id, kept):
        """Asserts the next keep_open event is con_id's, kept as given."""
        event = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "keep_open")
        self.assertEqual((event["container"]["id"], event["container"]["keep_open"]),
                         (con_id, kept))

    def test_a_window_kept_open_stays_when_killed_and_says_so(self):
        client, a = self.open("test.kept")
        self.assertFalse(self.windows()[a]["keep_open"])
        self.keep(a)
        self.kept(a, True)
        self.assertTrue(self.windows()[a]["keep_open"])
        self.assertEqual(self.fails(f"[con_id={a}] kill"), "The window is kept open")
        self.refused(a)
        # Without criteria, kill is the focused window's, as a key's is
        self.assertEqual(self.fails("kill"), "The window is kept open")
        self.refused(a)
        self.still_open(client)

    def test_it_closes_once_it_isnt_kept_open(self):
        _, a = self.open("test.kept")
        self.keep(a)
        self.keep(a, "enable")  # no change: no event
        self.keep(a, "toggle")
        self.kept(a, True)
        self.kept(a, False)
        self.assertFalse(self.windows()[a]["keep_open"])
        self.keep(a, "toggle")
        self.keep(a, "disable")
        self.kept(a, True)
        self.kept(a, False)
        self.command(self.connect(), f"[con_id={a}] kill")
        self.closed(a)

    def test_only_the_window_kept_open_stays(self):
        _, a = self.open("test.kept")
        _, b = self.open("test.other")
        self.keep(a)
        self.command(self.connect(), f"[con_id={b}] kill")
        self.closed(b)

    def test_moved_to_another_workspace_it_isnt_kept_open(self):
        _, a = self.open("test.kept")
        self.keep(a)
        here = self.windows()[a]["workspace"]
        self.command(self.connect(), f"[con_id={a}] move to workspace elsewhere")
        self.assertFalse(self.windows()[a]["keep_open"])
        # Back where it was kept open, it is again
        self.command(self.connect(), f"[con_id={a}] move to workspace {here}")
        self.assertTrue(self.windows()[a]["keep_open"])
        self.command(self.connect(), f"[con_id={a}] move to workspace elsewhere")
        self.command(self.connect(), f"[con_id={a}] kill")
        self.closed(a)

    def test_disabled_while_away_or_minimized_it_isnt_kept_open_back(self):
        _, a = self.open("test.kept")
        self.keep(a)
        here = self.windows()[a]["workspace"]
        self.command(self.connect(), f"[con_id={a}] move to workspace elsewhere")
        self.keep(a, "disable")
        self.command(self.connect(), f"[con_id={a}] move to workspace {here}")
        self.assertFalse(self.windows()[a]["keep_open"])
        self.keep(a)
        self.command(self.connect(), f"[con_id={a}] minimize")
        self.keep(a, "disable")
        self.command(self.connect(), f"[con_id={a}] focus")  # brings it back
        self.assertFalse(self.windows()[a]["keep_open"])
        self.command(self.connect(), f"[con_id={a}] kill")
        self.closed(a)

    def test_a_sticky_window_that_goes_with_a_switch_isnt_kept_open_there(self):
        _, a = self.open("test.kept")
        self.command(self.connect(), f"[con_id={a}] floating enable")
        self.command(self.connect(), f"[con_id={a}] sticky enable")
        self.keep(a)
        self.command(self.connect(), "workspace elsewhere")
        windows = self.windows()
        self.assertEqual(windows[a]["workspace"], "elsewhere", "it went with the switch")
        self.assertFalse(windows[a]["keep_open"])

    def test_a_dialog_that_follows_its_parent_away_isnt_kept_open(self):
        client, a = self.open("test.kept")
        client.stdin.write(b"dialog\n")
        client.stdin.flush()
        d = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                                and e["container"]["app_id"] == "test.kept.dialog")["container"]["id"]
        self.keep(a)
        self.keep(d)
        self.command(self.connect(), f"[con_id={a}] move to workspace elsewhere")
        windows = self.windows()
        self.assertEqual(windows[d]["workspace"], "elsewhere", "the dialog followed")
        self.assertEqual((windows[a]["keep_open"], windows[d]["keep_open"]), (False, False))

    def test_mapped_again_it_isnt_kept_open(self):
        client, a = self.open("test.kept")
        self.keep(a)
        client.stdin.write(b"remap\n")
        client.stdin.flush()
        self.closed(a)
        again = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new")["container"]
        self.assertEqual((again["id"], again["keep_open"]), (a, False))

    def test_minimized_it_closes_as_any_other(self):
        _, a = self.open("test.kept")
        self.keep(a)
        self.command(self.connect(), f"[con_id={a}] minimize")
        self.command(self.connect(), f"[con_id={a}] kill")
        self.closed(a)

    def test_keep_open_errors(self):
        _, a = self.open("test.kept")
        self.assertEqual(self.fails(f"[con_id={a}] keep_open maybe"),
                         "Expected keep_open enable|disable|toggle")
        self.assertEqual(self.fails("[con_id=999999] keep_open enable"),
                         "No window matches the criteria")


class KeptOpenKeysTest(KeepOpenHelpers):
    """An app's close key, bound --kept-open to kill: the window kept open
    never sees it, and every other window does."""
    CONFIG = "bindsym --kept-open Ctrl+w kill\n"

    def setUp(self):
        super().setUp()
        # The headless backend has no keyboard: the tests' is the seat's
        keyboard = subprocess.Popen([KEYBOARD, "0"], env=dict(self.wm.env, XKB_LAYOUT="us"),
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.wm.clients.append(keyboard)
        self.addCleanup(keyboard.stdout.close)
        self.addCleanup(keyboard.stdin.close)
        self.assertEqual(self.line(keyboard), b"switched\n")

    def ctrl(self, key):
        self.press("-M", "ctrl", "-k", key, "-m", "ctrl")

    def test_the_key_reaches_a_window_not_kept_open(self):
        client, _ = self.open("test.typist", keys=True)
        self.ctrl("w")
        self.assertEqual(self.line(client), b"key ctrl+w\n")

    def test_the_key_runs_its_binding_in_a_window_kept_open(self):
        bindings = self.connect()
        bindings.subscribe("binding")
        client, a = self.open("test.typist", keys=True)
        self.keep(a)
        self.ctrl("w")
        self.assertEqual(bindings.next_event(EVENT_BINDING)["binding"]["command"], "kill")
        self.refused(a)
        # Neither closed nor given the key: either would have come before
        # the ping's answer
        self.still_open(client)
        self.keep(a, "disable")
        self.ctrl("w")
        self.assertEqual(self.line(client), b"key ctrl+w\n")


if __name__ == "__main__":
    unittest.main()
