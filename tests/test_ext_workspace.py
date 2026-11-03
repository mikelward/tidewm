"""tidewm's workspaces over ext-workspace: one group on every output,
holding every workspace, which a client may activate."""

import unittest

from harness import (EVENT_WORKSPACE, GET_OUTPUTS, TidewmTest, ext_workspaces,
                     is_urgency, urgency)


class ExtWorkspaceTest(TidewmTest):
    def test_lists_the_workspaces_with_the_current_one_active(self):
        self.command(self.connect(), "persist workspace two")
        self.assertEqual(ext_workspaces(self.wm),
                         [("1", "active", 1), ("two", "inactive", 1)])

    def test_follows_a_switch_a_rename_and_a_removal(self):
        ipc = self.connect()
        self.command(ipc, "workspace mail")
        self.command(ipc, "rename workspace to post")
        self.assertEqual(ext_workspaces(self.wm), [("post", "active", 1)])

    def test_a_client_can_activate_one(self):
        self.command(self.connect(), "persist workspace two")
        ext_workspaces(self.wm, "--activate", "two")
        outputs = self.connect().request(GET_OUTPUTS)
        self.assertEqual(outputs[0]["current_workspace"], "two")

    def test_urgency_follows_an_urgent_window(self):
        ipc = self.connect()
        ipc.subscribe("window", "workspace")
        _, con = self.urgent_window_on(ipc, "away")
        self.assertEqual(ext_workspaces(self.wm)[1], ("away", "inactive,urgent", 1))
        self.command(self.connect(), f"[con_id={con['id']}] move to workspace further")
        self.assertEqual(ext_workspaces(self.wm), [("1", "active", 1), ("away", "inactive", 1),
                                                   ("further", "inactive,urgent", 1)])

    def test_urgency_ends_when_the_urgent_window_closes(self):
        ipc = self.connect()
        ipc.subscribe("window", "workspace")
        client, _ = self.urgent_window_on(ipc, "away")
        client.kill()
        self.assertEqual(urgency(ipc.next_event(EVENT_WORKSPACE, is_urgency)), ("away", False))
        self.assertEqual(ext_workspaces(self.wm), [("1", "active", 1), ("away", "inactive", 1)])


class ExtWorkspaceSpanningTest(TidewmTest):
    OUTPUTS = 2

    def test_the_group_is_on_every_output(self):
        self.assertEqual(ext_workspaces(self.wm), [("1", "active", 2)])


if __name__ == "__main__":
    unittest.main()
