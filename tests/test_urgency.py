"""Urgency marks (docs/ipc.md): each window marked urgent has tidewm's own
urgent_order, higher for a more recent mark, so a shell can list them most
recent first, as tide's Attention tab does; and sway's urgent command lets
the shell mark windows itself, as from a notification."""

import unittest

from harness import EVENT_WINDOW, GET_WORKSPACES, RUN_COMMAND, TidewmTest


class MarkTest(TidewmTest):
    """Helpers, with no tests of their own."""

    def setUp(self):
        super().setUp()
        self.ipc = self.connect()
        self.ipc.subscribe("window")

    def open(self, app_id):
        client = self.wm.open_window(app_id, app_id, stdin=True, stdout=True)
        con = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                                  and e["container"]["app_id"] == app_id)["container"]
        return con["id"], client

    def activate(self, client, con_id):
        client.stdin.write(b"activate\n")
        client.stdin.flush()
        self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "urgent"
                            and e["container"]["id"] == con_id)

    def mark(self, con_id, how="enable"):
        self.command(self.connect(), f"[con_id={con_id}] urgent {how}")

    def order(self, con_id):
        w = self.windows()[con_id]
        return w["urgent"], w["urgent_order"]

    def fails(self, command):
        result, = self.connect().request(RUN_COMMAND, command)
        self.assertFalse(result["success"], f"{command!r}: {result}")
        return result


class UrgencyTest(MarkTest):
    def test_marks_are_ordered_most_recent_highest(self):
        a, a_client = self.open("test.a")
        b, b_client = self.open("test.b")
        c, _ = self.open("test.c")  # focused, last to open
        self.activate(a_client, a)
        self.activate(b_client, b)
        (a_urgent, a_order), (b_urgent, b_order) = self.order(a), self.order(b)
        self.assertTrue(a_urgent and b_urgent)
        self.assertGreater(b_order, a_order)
        self.assertEqual(self.order(c), (False, 0))

    def test_a_window_marked_again_is_the_most_recent(self):
        a, a_client = self.open("test.a")
        b, b_client = self.open("test.b")
        self.open("test.c")
        self.activate(a_client, a)
        self.activate(b_client, b)
        self.activate(a_client, a)
        self.assertGreater(self.order(a)[1], self.order(b)[1])

    def test_the_shell_marks_a_window_and_takes_its_mark_away(self):
        a, _ = self.open("test.a")
        self.open("test.b")
        self.mark(a)
        event = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "urgent")
        self.assertEqual((event["container"]["id"], event["container"]["urgent"]), (a, True))
        self.assertTrue(self.order(a)[1] > 0)
        self.mark(a, "disable")
        event = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "urgent")
        self.assertEqual((event["container"]["id"], event["container"]["urgent"]), (a, False))
        self.assertEqual(self.order(a), (False, 0))

    def test_a_shell_mark_makes_its_workspace_urgent(self):
        a, _ = self.open("test.a")
        self.command(self.connect(), "persist workspace 1")
        self.command(self.connect(), "workspace two")
        self.mark(a)
        urgent = {w["name"]: w["urgent"] for w in self.connect().request(GET_WORKSPACES)}
        self.assertEqual(urgent, {"1": True, "two": False})
        self.mark(a, "disable")
        urgent = {w["name"]: w["urgent"] for w in self.connect().request(GET_WORKSPACES)}
        self.assertEqual(urgent, {"1": False, "two": False})

    def test_focusing_a_marked_window_takes_its_mark_away(self):
        a, _ = self.open("test.a")
        self.open("test.b")
        self.mark(a)
        self.command(self.connect(), f"[con_id={a}] focus")
        self.assertEqual(self.order(a), (False, 0))
        event = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "urgent")
        self.assertEqual((event["container"]["id"], event["container"]["urgent"]), (a, True))
        event = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "urgent")
        self.assertEqual((event["container"]["id"], event["container"]["urgent"]), (a, False))

    def test_the_urgent_command_takes_enable_or_disable(self):
        self.open("test.a")
        self.assertTrue(self.fails("urgent maybe")["parse_error"])
        self.assertIn("allow and deny aren't supported", self.fails("urgent allow")["error"])
        self.command(self.connect(), "workspace two")
        self.assertIn("No window to mark", self.fails("urgent enable")["error"])


class X11UrgencyTest(MarkTest):
    def test_an_x11_urgency_hint_is_a_new_mark_even_on_a_marked_window(self):
        self.need(self.wm.x11, "X11 windows need tidewm built with XWAYLAND")
        client = self.wm.open_x11_window("test.x11", "X11", stdin=True)
        x11 = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new" and e[
            "container"].get("window_properties", {}).get("class") == "test.x11")["container"]["id"]
        other, _ = self.open("test.other")
        self.open("test.focused")
        for con in (x11, other):
            self.mark(con)
            self.ipc.next_event(EVENT_WINDOW, lambda e, con=con: e["change"] == "urgent"
                                and e["container"]["id"] == con)
        client.stdin.write(b"urgent on\n")
        client.stdin.flush()
        self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "urgent"
                            and e["container"]["id"] == x11)
        self.assertGreater(self.order(x11)[1], self.order(other)[1])
        client.stdin.write(b"urgent off\n")
        client.stdin.flush()
        self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "urgent"
                            and e["container"]["id"] == x11)
        self.assertEqual(self.order(x11), (False, 0))


    def test_an_x11_hint_cleared_while_focused_is_followed(self):
        self.need(self.wm.x11, "X11 windows need tidewm built with XWAYLAND")
        client = self.wm.open_x11_window("test.x11", "X11", stdin=True)
        x11 = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new" and e[
            "container"].get("window_properties", {}).get("class") == "test.x11")["container"]["id"]
        other, _ = self.open("test.other")

        def send(line, change):
            client.stdin.write(line)
            client.stdin.flush()
            self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == change
                                and e["container"]["id"] == x11)

        send(b"urgent on\n", "urgent")
        self.command(self.connect(), f"[con_id={x11}] focus")
        # Cleared while it has focus; the title, after it, says tidewm heard
        client.stdin.write(b"urgent off\n")
        send(b"title Synced\n", "title")
        self.command(self.connect(), f"[con_id={other}] focus")
        send(b"urgent on\n", "urgent")
        self.assertTrue(self.order(x11)[0])


class GuardUrgencyTest(MarkTest):
    CONFIG = "focus_guard enable\n"

    def test_a_held_window_is_the_most_recent_mark(self):
        a, a_client = self.open("test.a")
        self.command(self.connect(), f"[con_id={a}] focus")
        b, b_client = self.open("test.b")  # held, and marked
        c, _ = self.open("test.c")
        self.assertGreater(self.order(c)[1], self.order(b)[1])
        self.assertTrue(self.order(b)[0])


if __name__ == "__main__":
    unittest.main()
