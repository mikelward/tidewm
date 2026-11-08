"""Dialogs and other floating windows (tide's SPEC §6.4): a window floats
when it has a parent, is modal, asks for a fixed size, or has neither an
app ID nor a title; one that floats as it opens, by those or a rule, goes
in the middle of its parent, or of its monitor's window area without one,
as big as it asks but no more than 80% of the monitor. A dialog stays above
its parent and follows it; picture-in-picture floats sticky in a corner."""

import os
import shutil
import subprocess
import unittest

from harness import (EVENT_WINDOW, EVENT_WORKSPACE, GET_OUTPUTS, GET_SEATS, GET_WORKSPACES,
                     ROOT, TIMEOUT, TidewmTest)

POINTER = os.path.join(ROOT, "tests", "pointer")


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

    def dialog(self, client, app_id, sub=False, title=None):
        """Opens the window's dialog, titled title, or with sub, that
        dialog's own."""
        client.stdin.write(b"subdialog\n" if sub else
                           b"dialog\n" if title is None else f"dialog {title}\n".encode())
        client.stdin.flush()
        name = app_id + (".dialog.dialog" if sub else ".dialog")
        return self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                                   and e["container"]["app_id"] == name)["container"]["id"]

    def clicked(self, con_id):
        """The window a click over con_id goes to: whichever is on top."""
        outputs = self.connect().request(GET_OUTPUTS)
        rect = self.window(con_id)["rect"]
        size = (max(o["rect"]["x"] + o["rect"]["width"] for o in outputs),
                max(o["rect"]["y"] + o["rect"]["height"] for o in outputs))
        subprocess.run([POINTER, "move", str(rect["x"] + 50), str(rect["y"] + 50),
                        str(size[0]), str(size[1]), "click"],
                       env=self.wm.env, check=True, timeout=TIMEOUT)
        seat, = self.connect().request(GET_SEATS)
        return seat["last_input"]["window"]

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


class FollowTest(DialogHelpers):
    CONFIG = 'for_window [app_id="^test.float$"] floating enable\n'

    def test_a_dialog_follows_its_parent_to_another_workspace(self):
        parent, client = self.open("test.app")
        dialog = self.dialog(client, "test.app")
        self.command(self.connect(), f"[con_id={parent}] move to workspace two")
        self.assertEqual(self.window(dialog)["workspace"], "two")

    def test_a_dialog_follows_to_the_middle_of_its_parents_new_tile(self):
        self.open("test.other")  # so the parent is half the area here
        parent, client = self.open("test.app")
        dialog = self.dialog(client, "test.app")
        self.command(self.connect(), f"[con_id={parent}] move to workspace two")
        self.command(self.connect(), "workspace two")
        windows = self.windows()
        self.assertEqual(windows[parent]["rect"], self.area())
        self.assertCenteredOn(windows[dialog]["rect"], windows[parent]["rect"])

    def test_a_workspace_a_following_dialog_leaves_goes_when_its_empty(self):
        parent, client = self.open("test.app")
        dialog = self.dialog(client, "test.app")
        self.command(self.connect(), f"[con_id={dialog}] move to workspace b")
        self.command(self.connect(), f"[con_id={parent}] move to workspace c")
        self.assertEqual(self.window(dialog)["workspace"], "c")
        self.assertNotIn("b", [w["name"] for w in self.connect().request(GET_WORKSPACES)])

    def test_a_sticky_dialog_stays_on_the_shown_workspace_when_its_parent_moves(self):
        parent, client = self.open("test.app")
        pip = self.dialog(client, "test.app", title="Picture-in-Picture")
        self.assertTrue(self.window(pip)["sticky"])
        self.command(self.connect(), f"[con_id={parent}] move to workspace two")
        self.assertEqual(self.window(pip)["workspace"], "1")

    def test_a_sticky_windows_dialog_comes_along_to_another_workspace(self):
        parent, client = self.open("test.float", size="600x400")
        dialog = self.dialog(client, "test.float")
        self.command(self.connect(), f"[con_id={parent}] sticky enable")
        self.command(self.connect(), "workspace two")
        windows = self.windows()
        self.assertEqual((windows[parent]["workspace"], windows[dialog]["workspace"]),
                         ("two", "two"))

    def test_a_dialog_stays_above_its_floating_parent(self):
        parent, client = self.open("test.float", size="600x400")
        dialog = self.dialog(client, "test.float")
        self.command(self.connect(), f"[con_id={parent}] focus")  # raises the parent
        self.assertEqual(self.clicked(dialog), dialog)

    def test_a_dialogs_dialog_stays_above_it_when_their_parent_is_raised(self):
        parent, client = self.open("test.float", size="600x400")
        dialog = self.dialog(client, "test.float")
        inner = self.dialog(client, "test.float", sub=True)
        self.command(self.connect(), f"[con_id={dialog}] focus")
        self.command(self.connect(), f"[con_id={parent}] focus")  # raises all three
        self.assertEqual(self.clicked(inner), inner)

    def test_closing_a_dialog_gives_its_parent_focus_back(self):
        self.open("test.other")
        parent, client = self.open("test.app")
        dialog = self.dialog(client, "test.app")
        self.assertTrue(self.window(dialog)["focused"])
        self.command(self.connect(), f"[con_id={dialog}] kill")
        self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "close"
                            and e["container"]["id"] == dialog)
        self.assertTrue(self.window(parent)["focused"])


class TwoOutputFollowTest(DialogHelpers):
    OUTPUTS = 2
    CONFIG = ('for_window [app_id="^test.float$"] floating enable\n'
              'bindsym Mod1+x persist workspace held\n')

    def other(self, con_id):
        """The output con_id isn't on, and the layout's size."""
        outputs = self.connect().request(GET_OUTPUTS)
        other, = [o for o in outputs if o["name"] != self.window(con_id)["output"]]
        size = (max(o["rect"]["x"] + o["rect"]["width"] for o in outputs),
                max(o["rect"]["y"] + o["rect"]["height"] for o in outputs))
        return other, size

    def test_a_dialogs_dialog_follows_to_the_middle_of_its_own_parent(self):
        parent, client = self.open("test.app")
        dialog = self.dialog(client, "test.app")
        inner = self.dialog(client, "test.app", sub=True)
        other, _ = self.other(parent)
        # A window there already, so the parent goes off the area's middle
        self.command(self.connect(), f"focus output {other['name']}")
        self.open("test.there")
        self.command(self.connect(), f"[con_id={parent}] move to output {other['name']}")
        windows = self.windows()
        self.assertNotEqual(windows[parent]["rect"], self.area())
        self.assertEqual({windows[i]["output"] for i in (parent, dialog, inner)}, {other["name"]})
        self.assertCenteredOn(windows[dialog]["rect"], windows[parent]["rect"])
        self.assertCenteredOn(windows[inner]["rect"], windows[dialog]["rect"])

    def move_toward(self, con_id, other):
        """Moves con_id onto other, the output beside its own, by direction."""
        here = self.window(con_id)["output"]
        here_x, = [o["rect"]["x"] for o in self.connect().request(GET_OUTPUTS) if o["name"] == here]
        way = "right" if other["rect"]["x"] > here_x else "left"
        self.command(self.connect(), f"[con_id={con_id}] move {way}")

    def test_a_dialog_follows_its_tile_moved_by_direction_to_its_final_place(self):
        parent, client = self.open("test.app")
        dialog = self.dialog(client, "test.app")
        other, _ = self.other(parent)
        # A tile there, whose place, the master's, the parent takes
        self.command(self.connect(), f"focus output {other['name']}")
        self.open("test.there")
        self.move_toward(parent, other)
        windows = self.windows()
        self.assertEqual((windows[parent]["output"], windows[parent]["rect"]["x"]),
                         (other["name"], other["rect"]["x"]))
        self.assertCenteredOn(windows[dialog]["rect"], windows[parent]["rect"])

    def test_a_dialog_follows_its_floating_parent_moved_by_direction(self):
        parent, client = self.open("test.float", size="600x400")
        dialog = self.dialog(client, "test.float")
        other, _ = self.other(parent)
        self.move_toward(parent, other)
        windows = self.windows()
        self.assertEqual(windows[parent]["output"], other["name"])
        self.assertCenteredOn(windows[dialog]["rect"], windows[parent]["rect"])

    def test_a_dialog_follows_its_parent_dragged_to_another_output(self):
        self.need(shutil.which("wtype"), "holding a key needs wtype")
        parent, client = self.open("test.float", size="600x400")
        dialog = self.dialog(client, "test.float")
        other, (width, height) = self.other(parent)
        workspaces = self.connect()
        workspaces.subscribe("workspace")
        # wtype holds Alt down while it waits on its input; its binding
        # says when it has
        typist = subprocess.Popen(["wtype", "-M", "alt", "-k", "x", "-"], env=self.wm.env,
                                  stdin=subprocess.PIPE)
        self.wm.clients.append(typist)
        workspaces.next_event(EVENT_WORKSPACE, lambda e: e["change"] == "init"
                              and e["current"]["name"] == "held")
        rect = self.window(parent)["rect"]
        # Alt+drag from the parent's corner, clear of its dialog
        subprocess.run([POINTER, "move", str(rect["x"] + 20), str(rect["y"] + 20),
                        str(width), str(height), "press", "left",
                        "move", str(other["rect"]["x"] + other["rect"]["width"] // 2),
                        str(other["rect"]["y"] + other["rect"]["height"] // 2),
                        str(width), str(height), "release", "left"],
                       env=self.wm.env, check=True, timeout=TIMEOUT)
        typist.stdin.close()
        typist.wait(TIMEOUT)
        windows = self.windows()
        self.assertEqual((windows[parent]["output"], windows[dialog]["output"]),
                         (other["name"], other["name"]))

    def test_a_dialog_on_a_hidden_workspace_shows_once_it_follows(self):
        parent, client = self.open("test.app")
        dialog = self.dialog(client, "test.app")
        other, _ = self.other(parent)
        self.command(self.connect(), f"[con_id={dialog}] move to output {other['name']}")
        self.command(self.connect(), f"[con_id={dialog}] move to workspace hidden")
        self.command(self.connect(), f"[con_id={parent}] move to output {other['name']}")
        self.assertEqual(self.window(dialog)["workspace"], "1")
        self.assertEqual(self.clicked(dialog), dialog)

    def test_an_urgent_dialogs_urgency_goes_with_it_when_it_follows(self):
        parent, client = self.open("test.app")
        dialog = self.dialog(client, "test.app")
        other, _ = self.other(parent)
        # The parent on hidden workspace a, its dialog on hidden b on the
        # other output, and urgent
        self.command(self.connect(), f"[con_id={parent}] move to workspace a")
        self.command(self.connect(), f"[con_id={dialog}] move to output {other['name']}")
        self.command(self.connect(), f"[con_id={dialog}] move to workspace b")
        self.command(self.connect(), f"[con_id={dialog}] urgent enable")
        workspaces = self.connect()
        workspaces.subscribe("workspace")
        self.command(self.connect(), f"[con_id={parent}] move to output {other['name']}")
        # A new workspace's event comes after the move's
        self.command(self.connect(), "persist workspace end")
        changes = {}
        while (event := workspaces.next_event(EVENT_WORKSPACE))["change"] != "init":
            if event["change"] == "urgent":
                changes[event["current"]["name"]] = event["current"]["urgent"]
        self.assertEqual(self.window(dialog)["workspace"], "a")
        self.assertEqual(changes, {"a": True})

    def test_a_dialog_follows_its_parent_to_another_output_to_its_middle(self):
        parent, client = self.open("test.app")
        dialog = self.dialog(client, "test.app")
        other, _ = self.other(parent)
        self.command(self.connect(), f"[con_id={parent}] move to output {other['name']}")
        windows = self.windows()
        self.assertEqual((windows[parent]["output"], windows[dialog]["output"]),
                         (other["name"], other["name"]))
        self.assertCenteredOn(windows[dialog]["rect"], windows[parent]["rect"])


if __name__ == "__main__":
    unittest.main()
