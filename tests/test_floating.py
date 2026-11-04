"""A floating window's place, remembered for its app for the session
(tide's SPEC §6.4): floating a window of that app by hand puts it where the
app's last floating window was. Dialogs, fixed-size windows and
picture-in-picture place themselves, so theirs isn't remembered, and a
window that floats as it opens still goes in the middle."""

import contextlib
import os
import shutil
import subprocess
import unittest

from harness import (EVENT_WINDOW, EVENT_WORKSPACE, GET_OUTPUTS, GET_WORKSPACES, ROOT, TIMEOUT,
                     TidewmTest)

POINTER = os.path.join(ROOT, "tests", "pointer")


class FloatHelpers(TidewmTest):
    """Helpers, with no tests of their own."""
    CONFIG = ('for_window [app_id="^test.float$"] floating enable\n'
              'bindsym Mod1+x persist workspace held\n')

    def setUp(self):
        super().setUp()
        self.ipc = self.connect()
        self.ipc.subscribe("window")
        self.client_of = {}

    def open(self, app_id, title=None, **kwargs):
        client = self.wm.open_window(app_id, app_id if title is None else title, **kwargs)
        con_id = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                                     and e["container"]["app_id"] == app_id)["container"]["id"]
        self.client_of[con_id] = client
        return con_id

    def close(self, con_id):
        self.command(self.connect(), f"[con_id={con_id}] kill")
        self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "close"
                            and e["container"]["id"] == con_id)

    def float(self, con_id, floating="enable"):
        self.command(self.connect(), f"[con_id={con_id}] floating {floating}")

    def rect(self, con_id):
        return self.windows()[con_id]["rect"]

    def area(self):
        ws, = [w for w in self.connect().request(GET_WORKSPACES) if w["focused"]]
        return ws["rect"]

    @contextlib.contextmanager
    def alt(self):
        """Holds Alt down, as wtype does while it waits on its input; its
        binding says when it has, once a test."""
        self.need(shutil.which("wtype"), "holding a key needs wtype")
        workspaces = self.connect()
        workspaces.subscribe("workspace")
        typist = subprocess.Popen(["wtype", "-M", "alt", "-k", "x", "-"], env=self.wm.env,
                                  stdin=subprocess.PIPE)
        self.wm.clients.append(typist)
        workspaces.next_event(EVENT_WORKSPACE, lambda e: e["change"] == "init"
                              and e["current"]["name"] == "held")
        yield
        typist.stdin.close()
        typist.wait(TIMEOUT)

    def drag(self, con_id, button, x, y):
        """Drags con_id from 20 px into it to x, y, with Alt held: the left
        button moves it there, less those 20 px, and the right one resizes
        it to end there."""
        outputs = self.connect().request(GET_OUTPUTS)
        size = [str(max(o["rect"]["x"] + o["rect"]["width"] for o in outputs)),
                str(max(o["rect"]["y"] + o["rect"]["height"] for o in outputs))]
        rect = self.rect(con_id)
        subprocess.run([POINTER, "move", str(rect["x"] + 20), str(rect["y"] + 20), *size,
                        "press", button, "move", str(x), str(y), *size, "release", button],
                       env=self.wm.env, check=True, timeout=TIMEOUT)

    def place(self, con_id):
        """Moves con_id 100 px into the window area each way, and makes it
        300x200, by drags, with Alt held; returns its place."""
        area = self.area()
        self.drag(con_id, "left", area["x"] + 120, area["y"] + 120)
        rect = self.rect(con_id)
        self.drag(con_id, "right", rect["x"] + 300, rect["y"] + 200)
        rect = self.rect(con_id)
        self.assertEqual((rect["x"] - area["x"], rect["y"] - area["y"]), (100, 100))
        return rect

    def assertInside(self, rect, area):
        self.assertTrue(area["x"] <= rect["x"] and rect["x"] + rect["width"] <= area["x"]
                        + area["width"] and area["y"] <= rect["y"] and rect["y"]
                        + rect["height"] <= area["y"] + area["height"], (rect, area))


class FloatMemoryTest(FloatHelpers):

    def test_floating_a_window_by_hand_puts_it_where_it_last_floated(self):
        window = self.open("test.app")
        self.float(window)
        with self.alt():
            place = self.place(window)
        self.float(window, "disable")
        self.assertNotEqual(self.rect(window), place)
        self.float(window)
        self.assertEqual(self.rect(window), place)

    def test_floating_a_window_by_hand_puts_it_where_its_app_last_floated(self):
        first = self.open("test.app")
        self.float(first)
        with self.alt():
            place = self.place(first)
        self.close(first)
        second = self.open("test.app")
        self.float(second, "toggle")
        self.assertEqual(self.rect(second), place)

    def test_a_window_bigger_than_its_apps_place_stays_inside_the_area(self):
        first = self.open("test.app")
        self.float(first)
        area = self.area()
        # 300x200 at the area's right edge
        with self.alt():
            self.drag(first, "left", area["x"] + area["width"] - 300, area["y"] + 120)
            rect = self.rect(first)
            self.drag(first, "right", rect["x"] + 300, rect["y"] + 200)
        self.close(first)
        # At least 600 wide, so it would reach past that edge
        second = self.open("test.app", min_size="600x400")
        self.float(second)
        rect = self.rect(second)
        self.assertGreaterEqual(rect["width"], 600)
        self.assertInside(rect, area)

    def test_a_window_a_rule_floats_still_opens_in_the_middle(self):
        first = self.open("test.float", size="300x200")
        with self.alt():
            self.place(first)
        self.close(first)
        second = self.open("test.float", size="300x200")
        rect, area = self.rect(second), self.area()
        for axis, size in (("x", "width"), ("y", "height")):
            self.assertLessEqual(abs(rect[axis] + rect[size] / 2 - area[axis] - area[size] / 2),
                                 1, (rect, area))

    def test_another_window_of_the_app_redrawing_leaves_the_apps_place(self):
        first = self.open("test.app", stdin=True, stdout=True)
        self.float(first)
        area = self.area()
        with self.alt():
            self.place(first)
            second = self.open("test.app")
            self.float(second)
            self.drag(second, "left", area["x"] + 220, area["y"] + 170)
        place = self.rect(second)
        # The first draws itself again, where it is
        client = self.client_of[first]
        client.stdin.write(b"commit\n")
        client.stdin.flush()
        while (line := client.stdout.readline()) != b"committed\n":
            self.assertTrue(line, "the client went before it committed")
        self.float(second, "disable")
        self.float(second)
        self.assertEqual(self.rect(second), place)

    def assertNotRemembered(self, **kwargs):
        """A window of test.app opened with kwargs, which floats by itself,
        is moved and closed; floating another by hand leaves it in its
        tile."""
        odd = self.open("test.app", **kwargs)
        self.assertEqual(self.windows()[odd]["type"], "floating_con")
        area = self.area()
        with self.alt():
            self.drag(odd, "left", area["x"] + 120, area["y"] + 120)
        self.close(odd)
        window = self.open("test.app")
        tiled = self.rect(window)
        self.float(window)
        self.assertEqual(self.rect(window), tiled)

    def test_a_modal_dialogs_place_isnt_remembered(self):
        self.assertNotRemembered(modal=True)

    def test_a_fixed_size_windows_place_isnt_remembered(self):
        self.assertNotRemembered(min_size="300x200", max_size="300x200")

    def test_picture_in_pictures_place_isnt_remembered(self):
        self.assertNotRemembered(title="Picture-in-Picture")


class TwoOutputFloatMemoryTest(FloatHelpers):
    OUTPUTS = 2

    def test_a_window_dragged_to_another_output_floats_where_it_was_dropped(self):
        window = self.open("test.app")
        self.float(window)
        outputs = self.connect().request(GET_OUTPUTS)
        other, = [o for o in outputs if o["name"] != self.windows()[window]["output"]]
        # 300x200 first, so it fits where it's dropped
        with self.alt():
            self.place(window)
            self.drag(window, "left", other["rect"]["x"] + 220, other["rect"]["y"] + 170)
        dropped = self.rect(window)
        self.assertEqual(self.windows()[window]["output"], other["name"])
        self.float(window, "disable")
        self.float(window)
        self.assertEqual(self.rect(window), dropped)


class BuiltInRuleFloatMemoryTest(FloatHelpers):
    """config.h's rules, with no config file: they float a window from its
    first commit, before it maps."""
    CONFIG = None

    def test_a_window_opening_maximized_leaves_its_apps_place(self):
        first = self.open("Gimp_EXAMPLE", size="300x200")
        place = self.rect(first)
        second = self.open("Gimp_EXAMPLE", maximized=True)
        self.close(second)
        self.float(first, "disable")
        self.float(first)
        self.assertEqual(self.rect(first), place)


if __name__ == "__main__":
    unittest.main()
