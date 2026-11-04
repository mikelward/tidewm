"""Dialogs and other floating windows (tide's SPEC §6.4): a window floats
when it has a parent, is modal, asks for a fixed size, or has neither an
app ID nor a title; one that floats as it opens, by those or a rule, goes
in the middle of its parent, or of its monitor's window area without one,
as big as it asks but no more than 80% of the monitor."""

import unittest

from harness import EVENT_WINDOW, GET_OUTPUTS, GET_WORKSPACES, TidewmTest


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


if __name__ == "__main__":
    unittest.main()
