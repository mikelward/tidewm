"""Dialogs and other floating windows (tide's SPEC §6.4): a window floats
when it has a parent, is modal, asks for a fixed size, or has neither an
app ID nor a title; one that floats as it opens, by those or a rule, goes
in the middle of its parent, or of its monitor's window area without one,
as big as it asks but no more than 80% of the monitor."""

import unittest

from harness import EVENT_WINDOW, EVENT_WORKSPACE, GET_OUTPUTS, GET_WORKSPACES, TidewmTest


class DialogHelpers(TidewmTest):
    """Helpers, with no tests of their own."""

    def setUp(self):
        super().setUp()
        self.ipc = self.connect()
        self.ipc.subscribe("window")

    def open(self, app_id, title=None, **kwargs):
        client = self.wm.open_window(app_id, app_id if title is None else title, stdin=True,
                                     **kwargs)
        con = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                                  and e["container"]["app_id"] == app_id)["container"]
        return con["id"], client

    def dialog(self, client, app_id):
        client.stdin.write(b"dialog\n")
        client.stdin.flush()
        return self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                                   and e["container"]["app_id"] == app_id + ".dialog"
                                   )["container"]["id"]

    def window(self, con_id):
        return self.windows()[con_id]

    def area(self):
        ws, = [w for w in self.connect().request(GET_WORKSPACES) if w["focused"]]
        return ws["rect"]

    def assertCenteredOn(self, rect, around):
        self.assertLessEqual(abs(rect["x"] + rect["width"] / 2 - around["x"]
                                 - around["width"] / 2), 1, (rect, around))
        self.assertLessEqual(abs(rect["y"] + rect["height"] / 2 - around["y"]
                                 - around["height"] / 2), 1, (rect, around))


class PlacementTest(DialogHelpers):
    def test_a_dialog_opens_in_the_middle_of_its_parent(self):
        self.open("test.left")
        parent, client = self.open("test.right")  # the stack's, on the right
        dialog = self.dialog(client, "test.right")
        window = self.window(dialog)
        self.assertEqual(window["type"], "floating_con")
        self.assertCenteredOn(window["rect"], self.window(parent)["rect"])

    def test_a_window_a_rule_floats_opens_in_the_middle_of_the_window_area(self):
        self.open("test.tiled")
        window, _ = self.open("test.float")
        self.assertCenteredOn(self.window(window)["rect"], self.area())

    def test_a_floating_window_is_no_bigger_than_80_percent_of_the_monitor(self):
        window, _ = self.open("test.float", size="4000x3000")
        output, = self.connect().request(GET_OUTPUTS)
        rect = self.window(window)["rect"]
        self.assertEqual((rect["width"], rect["height"]),
                         (output["rect"]["width"] * 4 // 5, output["rect"]["height"] * 4 // 5))
        self.assertCenteredOn(rect, self.area())

    def test_a_smaller_one_keeps_the_size_it_asks(self):
        window, _ = self.open("test.float", size="300x200")
        rect = self.window(window)["rect"]
        border = rect["width"] - 300
        self.assertEqual(rect["height"] - 200, border)
        self.assertLessEqual(border, 10)

    def test_a_fixed_size_dialog_bigger_than_the_cap_is_centered_at_its_size(self):
        window, _ = self.open("test.fixed", min_size="1200x700", max_size="1200x700")
        rect = self.window(window)["rect"]
        border = rect["width"] - 1200
        self.assertEqual((self.window(window)["type"], rect["height"] - 700),
                         ("floating_con", border))
        self.assertCenteredOn(rect, self.area())

    def test_a_window_named_before_it_maps_isnt_a_status_popup(self):
        self.wm.open_window("", "", mapped_app_id="test.named")
        window = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                                     and e["container"]["app_id"] == "test.named")
        self.assertEqual(self.window(window["container"]["id"])["type"], "con")

    def test_a_modal_dialog_floats(self):
        window, _ = self.open("test.modal", modal=True)
        self.assertEqual(self.window(window)["type"], "floating_con")
        self.assertCenteredOn(self.window(window)["rect"], self.area())

    def test_a_window_with_neither_app_id_nor_title_floats(self):
        window, _ = self.open("", title="")
        self.assertEqual(self.window(window)["type"], "floating_con")

    def test_an_ordinary_window_still_tiles(self):
        window, _ = self.open("test.app")
        self.assertEqual(self.window(window)["type"], "con")

    CONFIG = 'for_window [app_id="^test.float$"] floating enable\n'



class PipTest(DialogHelpers):
    def test_picture_in_picture_floats_in_the_bottom_right_corner(self):
        self.open("test.app")
        window, _ = self.open("test.pip", title="Picture-in-Picture", size="640x360")
        output, = self.connect().request(GET_OUTPUTS)
        w, area = self.window(window), self.area()
        rect = w["rect"]
        self.assertEqual((w["type"], w["sticky"]), ("floating_con", True))
        self.assertEqual((rect["width"], rect["height"]),
                         (output["rect"]["width"] // 4, output["rect"]["height"] // 4))
        self.assertEqual((rect["x"] + rect["width"], rect["y"] + rect["height"]),
                         (area["x"] + area["width"] - 16, area["y"] + area["height"] - 16))

    def test_picture_in_picture_is_on_every_workspace(self):
        window, _ = self.open("test.pip", title="Picture in picture")
        self.command(self.connect(), "workspace two")
        w = self.window(window)
        self.assertEqual((w["workspace"], w["visible"]), ("two", True))

    def test_a_dialog_that_is_picture_in_picture_is_sticky_too(self):
        _, client = self.open("test.app")
        client.stdin.write(b"dialog Picture-in-Picture\n")
        client.stdin.flush()
        pip = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                                  and e["container"]["app_id"] == "test.app.dialog")
        self.assertTrue(self.window(pip["container"]["id"])["sticky"])

    def test_a_dialog_titled_picture_in_picture_as_it_maps_is_sticky(self):
        # Its first commit, where the rules run for a Wayland window, comes
        # before the title; an X11 window has no first commit at all. The
        # map is where a dialog's stickiness is decided.
        _, client = self.open("test.app")
        client.stdin.write(b"retitled-dialog Picture-in-Picture\n")
        client.stdin.flush()
        pip = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                                  and e["container"]["app_id"] == "test.app.dialog")
        self.assertTrue(self.window(pip["container"]["id"])["sticky"])

    def test_other_titles_arent_picture_in_picture(self):
        for title in ("Picture-in-Picture video", "picture-in-picture"):
            window, _ = self.open("test." + title.replace(" ", "."), title=title)
            self.assertEqual(self.window(window)["type"], "con", title)

    def test_the_sticky_command_pins_a_floating_window(self):
        self.open("test.app")
        window, _ = self.open("test.float")
        self.command(self.connect(), f"[con_id={window}] sticky enable")
        self.command(self.connect(), "workspace two")
        self.assertEqual(self.window(window)["workspace"], "two")
        self.command(self.connect(), f"[con_id={window}] sticky toggle")
        self.command(self.connect(), "workspace 1")
        self.assertEqual((self.window(window)["workspace"], self.window(window)["sticky"]),
                         ("two", False))

    def test_a_sticky_tiled_window_stays_on_its_workspace(self):
        window, _ = self.open("test.app")
        self.command(self.connect(), f"[con_id={window}] sticky enable")
        self.command(self.connect(), "workspace two")
        self.assertEqual(self.window(window)["workspace"], "1")

    CONFIG = 'for_window [app_id="^test.float$"] floating enable\n'



class TwoOutputStickyTest(DialogHelpers):
    OUTPUTS = 2
    CONFIG = 'for_window [app_id="^test.float$"] floating enable\n'

    def test_an_urgent_sticky_window_takes_its_urgency_along(self):
        outputs = [o["name"] for o in self.connect().request(GET_OUTPUTS)]
        self.command(self.connect(), f"focus output {outputs[1]}")
        sticky, _ = self.open("test.float")
        self.command(self.connect(), f"[con_id={sticky}] sticky enable")
        self.command(self.connect(), f"focus output {outputs[0]}")
        self.open("test.app")  # keeps workspace 1, and the focus on its output
        self.command(self.connect(), f"[con_id={sticky}] urgent enable")
        workspaces = self.connect()
        workspaces.subscribe("workspace")
        self.command(self.connect(), "workspace two")
        # The urgency events come before the switch's focus one
        changes = {}
        while True:
            event = workspaces.next_event(EVENT_WORKSPACE)
            if event["change"] == "focus":
                break
            if event["change"] == "urgent":
                changes[event["current"]["name"]] = event["current"]["urgent"]
        self.assertEqual(changes, {"1": False, "two": True})


if __name__ == "__main__":
    unittest.main()
