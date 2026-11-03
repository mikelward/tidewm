"""tidewm's workspaces: named, made on demand, gone once empty and left,
and spanning every monitor (tide's SPEC.md, section 6.5)."""

import unittest

from harness import (EVENT_TICK, EVENT_WINDOW, GET_OUTPUTS, GET_WORKSPACES,
                     RUN_COMMAND, SEND_TICK, TIMEOUT, TidewmTest)

EVENT_WORKSPACE = 0x80000000


def is_urgency(event):
    return event["change"] == "urgent"


def urgency(event):
    return event["current"]["name"], event["current"]["urgent"]


class WorkspaceTest(TidewmTest):
    def test_tidewm_starts_on_workspace_1(self):
        workspace, = self.connect().request(GET_WORKSPACES)
        self.assertEqual(workspace["name"], "1")
        self.assertEqual(workspace["num"], 1)
        self.assertTrue(workspace["focused"])
        self.assertTrue(workspace["visible"])
        self.assertEqual(workspace["output"], "HEADLESS-1")

    def test_switching_makes_a_named_workspace_and_drops_the_empty_one(self):
        ipc = self.connect()
        ipc.subscribe("workspace")
        self.command(self.connect(), "workspace mail")
        init = ipc.next_event(EVENT_WORKSPACE)
        self.assertEqual((init["change"], init["current"]["name"]), ("init", "mail"))
        self.assertEqual(init["current"]["num"], -1)
        focus = ipc.next_event(EVENT_WORKSPACE)
        self.assertEqual(focus["change"], "focus")
        self.assertEqual(focus["current"]["name"], "mail")
        self.assertEqual(focus["old"]["name"], "1")
        empty = ipc.next_event(EVENT_WORKSPACE)
        self.assertEqual((empty["change"], empty["current"]["name"]), ("empty", "1"))
        self.assertEqual(self.workspace_names(), ["mail"])

    def test_a_name_can_hold_spaces(self):
        self.command(self.connect(), "workspace PR-12 login")
        self.assertEqual(self.workspace_names(), ["PR-12 login"])

    def test_an_empty_name_fails(self):
        reply = self.connect().request(RUN_COMMAND, "workspace")
        self.assertFalse(reply[0]["success"])
        self.assertEqual(self.workspace_names(), ["1"])

    def test_workspace_number_names_it_by_number(self):
        self.command(self.connect(), "workspace number 3")
        workspace, = self.connect().request(GET_WORKSPACES)
        self.assertEqual((workspace["name"], workspace["num"]), ("3", 3))

    def test_a_workspace_with_windows_stays_and_hides_them(self):
        ipc = self.connect()
        ipc.subscribe("window")
        _, con = self.open_window(ipc, "test.stays")
        self.command(self.connect(), "workspace other")
        self.assertEqual(self.workspace_names(), ["1", "other"])
        self.assertFalse(self.windows()[con["id"]]["visible"])
        self.command(self.connect(), "workspace 1")
        window = self.windows()[con["id"]]
        self.assertTrue(window["visible"])
        self.assertTrue(window["focused"])
        self.assertEqual(self.workspace_names(), ["1"])

    def test_a_new_window_opens_on_the_current_workspace(self):
        self.command(self.connect(), "workspace code")
        ipc = self.connect()
        ipc.subscribe("window")
        _, con = self.open_window(ipc, "test.here")
        self.assertEqual(con["workspace"], "code")

    # config.def.h's example rule puts app ID "firefox_EXAMPLE" on workspace 9
    def test_a_rule_opens_a_window_on_its_workspace(self):
        ipc = self.connect()
        ipc.subscribe("window")
        _, con = self.open_window(ipc, "firefox_EXAMPLE")
        self.assertEqual(con["workspace"], "9")
        self.assertEqual(self.workspace_names(), ["1", "9"])

    def test_a_rule_a_window_stops_matching_before_it_maps_makes_no_workspace(self):
        ipc = self.connect()
        ipc.subscribe("window")
        self.wm.open_window("firefox_EXAMPLE", "Renamed", mapped_app_id="test.renamed")
        new = ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new")
        self.assertEqual((new["container"]["app_id"], new["container"]["workspace"]),
                         ("test.renamed", "1"))
        self.assertEqual(self.workspace_names(), ["1"])

    def test_moving_a_window_makes_its_workspace_and_keeps_the_view(self):
        ipc = self.connect()
        ipc.subscribe("window")
        _, con = self.open_window(ipc, "test.moved")
        self.command(self.connect(), "move container to workspace later")
        move = ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "move")
        self.assertEqual(move["container"]["id"], con["id"])
        self.assertEqual(move["container"]["workspace"], "later")
        self.assertEqual(self.workspace_names(), ["1", "later"])
        self.assertFalse(self.windows()[con["id"]]["visible"])

    def test_criteria_pick_the_window_a_command_moves(self):
        ipc = self.connect()
        ipc.subscribe("window")
        _, first = self.open_window(ipc, "test.first")
        _, second = self.open_window(ipc, "test.second")
        self.command(self.connect(), f"[con_id={first['id']}] move to workspace away")
        windows = self.windows()
        self.assertEqual(windows[first["id"]]["workspace"], "away")
        self.assertEqual(windows[second["id"]]["workspace"], "1")

    def test_criteria_for_a_window_that_is_gone_fail(self):
        reply = self.connect().request(RUN_COMMAND, "[con_id=999] kill")
        self.assertFalse(reply[0]["success"])

    def test_moving_with_no_window_fails(self):
        reply = self.connect().request(RUN_COMMAND, "move container to workspace x")
        self.assertFalse(reply[0]["success"])
        self.assertEqual(self.workspace_names(), ["1"])

    def test_focusing_a_window_shows_its_workspace(self):
        ipc = self.connect()
        ipc.subscribe("window")
        _, con = self.open_window(ipc, "test.far")
        self.command(self.connect(), "move container to workspace far")
        self.command(self.connect(), f"[con_id={con['id']}] focus")
        window = self.windows()[con["id"]]
        self.assertTrue(window["visible"])
        self.assertTrue(window["focused"])
        self.assertEqual(self.workspace_names(), ["far"])

    def test_kill_closes_the_window(self):
        ipc = self.connect()
        ipc.subscribe("window")
        client, con = self.open_window(ipc, "test.killed")
        self.command(self.connect(), f"[con_id={con['id']}] kill")
        close = ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "close")
        self.assertEqual(close["container"]["id"], con["id"])
        self.assertEqual(client.wait(TIMEOUT), 0)

    def test_back_and_forth(self):
        ipc = self.connect()
        self.command(ipc, "persist workspace 1")
        self.command(ipc, "workspace two")
        self.command(ipc, "workspace back_and_forth")
        self.assertEqual(self.connect().request(GET_OUTPUTS)[0]["current_workspace"], "1")

    def test_back_and_forth_with_nowhere_to_go_fails(self):
        reply = self.connect().request(RUN_COMMAND, "workspace back_and_forth")
        self.assertFalse(reply[0]["success"])

    def test_a_persistent_workspace_stays_empty_until_released(self):
        ipc = self.connect()
        self.command(ipc, "persist workspace home")
        self.command(ipc, "workspace home")
        self.command(ipc, "workspace elsewhere")
        self.assertEqual(self.workspace_names(), ["home", "elsewhere"])
        self.command(ipc, "unpersist workspace home")
        self.assertEqual(self.workspace_names(), ["elsewhere"])

    def urgent_window_on(self, ipc, workspace):
        """Opens a window on a persistent workspace, not the current one,
        and has it ask to be activated, which makes it urgent. ipc must be
        subscribed to window and workspace events."""
        client = self.wm.open_window("test.urgent", "Urgent", stdin=True)
        con = ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new")["container"]
        self.command(self.connect(), f"persist workspace {workspace}")
        self.command(self.connect(), f"[con_id={con['id']}] move to workspace {workspace}")
        client.stdin.write(b"activate\n")
        client.stdin.flush()
        self.assertEqual(urgency(ipc.next_event(EVENT_WORKSPACE, is_urgency)), (workspace, True))
        return client, con

    def test_an_urgent_window_takes_its_urgency_to_another_workspace(self):
        ipc = self.connect()
        ipc.subscribe("window", "workspace")
        _, con = self.urgent_window_on(ipc, "away")
        self.command(self.connect(), f"[con_id={con['id']}] move to workspace further")
        events = [ipc.next_event(EVENT_WORKSPACE, is_urgency) for _ in range(2)]
        self.assertEqual({urgency(e) for e in events}, {("further", True), ("away", False)})
        urgent = {w["name"]: w["urgent"] for w in self.connect().request(GET_WORKSPACES)}
        self.assertEqual(urgent, {"1": False, "away": False, "further": True})

    def test_closing_an_urgent_window_ends_its_workspaces_urgency(self):
        ipc = self.connect()
        ipc.subscribe("window", "workspace")
        client, _ = self.urgent_window_on(ipc, "away")
        client.kill()
        self.assertEqual(urgency(ipc.next_event(EVENT_WORKSPACE, is_urgency)), ("away", False))

    def test_rename_the_current_workspace_or_a_named_one(self):
        ipc = self.connect()
        ipc.subscribe("workspace")
        self.command(self.connect(), "rename workspace to in")
        rename = ipc.next_event(EVENT_WORKSPACE)
        self.assertEqual((rename["change"], rename["current"]["name"]), ("rename", "in"))
        self.command(self.connect(), "persist workspace spare")
        self.command(self.connect(), "rename workspace spare to extra")
        self.assertEqual(self.workspace_names(), ["in", "extra"])

    def test_rename_to_a_taken_name_fails(self):
        ipc = self.connect()
        self.command(ipc, "persist workspace spare")
        reply = ipc.request(RUN_COMMAND, "rename workspace to spare")
        self.assertFalse(reply[0]["success"])
        self.assertEqual(self.workspace_names(), ["1", "spare"])


class SpanningTest(TidewmTest):
    """With two monitors, a workspace spans both."""

    OUTPUTS = 2

    def setUp(self):
        super().setUp()
        # wlroots lays the outputs out side by side, and the pointer, so
        # focus, starts on the left one.
        outputs = sorted(self.connect().request(GET_OUTPUTS), key=lambda o: o["rect"]["x"])
        self.left, self.right = (o["name"] for o in outputs)

    def current_workspaces(self):
        return {o["name"]: o["current_workspace"]
                for o in self.connect().request(GET_OUTPUTS)}

    def focused_outputs(self):
        return [o["name"] for o in self.connect().request(GET_OUTPUTS) if o["focused"]]

    def test_focus_starts_on_the_left_output(self):
        self.assertEqual(self.focused_outputs(), [self.left])

    def test_every_monitor_shows_the_current_workspace(self):
        self.assertEqual(self.current_workspaces(), {self.left: "1", self.right: "1"})
        self.command(self.connect(), "workspace both")
        self.assertEqual(self.current_workspaces(), {self.left: "both", self.right: "both"})

    def test_a_window_moves_to_the_next_monitors_part_of_its_workspace(self):
        ipc = self.connect()
        ipc.subscribe("window", "tick")
        _, con = self.open_window(ipc, "test.across")
        self.assertEqual(con["output"], self.left)
        self.command(self.connect(), "move container to output right")
        # A tick after the command marks where its events end
        self.connect().request(SEND_TICK, "moved")
        moves = []
        while (kind_event := ipc.recv())[0] != EVENT_TICK:
            if kind_event[0] == EVENT_WINDOW and kind_event[1]["change"] == "move":
                moves.append(kind_event[1]["container"])
        self.assertEqual([(m["id"], m["output"], m["workspace"]) for m in moves],
                         [(con["id"], self.right, "1")])
        self.assertTrue(self.windows()[con["id"]]["visible"])

    def test_a_window_moved_by_a_key_says_so(self):
        ipc = self.connect()
        ipc.subscribe("window")
        _, con = self.open_window(ipc, "test.keyed")
        self.press("-M", "alt", "-M", "shift", "-k", "period", "-m", "shift", "-m", "alt")
        move = ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "move")
        self.assertEqual((move["container"]["id"], move["container"]["output"]),
                         (con["id"], self.right))

    def test_switching_hides_and_shows_each_monitors_part(self):
        ipc = self.connect()
        ipc.subscribe("window")
        _, con = self.open_window(ipc, "test.part")
        self.command(self.connect(), f"move container to output {self.right}")
        self.command(self.connect(), "workspace other")
        self.assertFalse(self.windows()[con["id"]]["visible"])
        self.command(self.connect(), "workspace 1")
        window = self.windows()[con["id"]]
        self.assertTrue(window["visible"])
        self.assertEqual(window["output"], self.right)

    def test_focus_stays_on_its_monitor_across_a_switch(self):
        self.command(self.connect(), "focus output right")
        self.assertEqual(self.focused_outputs(), [self.right])
        self.command(self.connect(), "workspace other")
        self.assertEqual(self.focused_outputs(), [self.right])

    def test_a_workspace_lists_once(self):
        self.command(self.connect(), "workspace both")
        self.assertEqual(self.workspace_names(), ["both"])


if __name__ == "__main__":
    unittest.main()
