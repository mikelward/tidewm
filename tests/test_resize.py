"""sway's resize grow|shrink (docs/ipc.md), for tide's resize mode: a
floating window in pixels, about its center or by the side named, and a
tile by its column's share of the window area, mfact."""

import unittest

from harness import RUN_COMMAND, TidewmTest


class ResizeTest(TidewmTest):
    CONFIG = 'for_window [app_id="^test.float$"] floating enable\n'

    def setUp(self):
        super().setUp()
        self.ipc = self.connect()
        self.ipc.subscribe("window")

    def open(self, app_id):
        _, con = self.open_window(self.ipc, app_id)
        return con["id"]

    def rect(self, con_id):
        return self.windows()[con_id]["rect"]

    def changed(self, con_id, command):
        """Runs command on the window, and returns how its x, y, width and
        height changed."""
        before = self.rect(con_id)
        self.command(self.connect(), f"[con_id={con_id}] {command}")
        after = self.rect(con_id)
        return tuple(after[k] - before[k] for k in ("x", "y", "width", "height"))

    def fails(self, command):
        result, = self.connect().request(RUN_COMMAND, command)
        self.assertFalse(result["success"], f"{command!r}: {result}")
        return result


class FloatingResizeTest(ResizeTest):

    def test_width_and_height_keep_the_center(self):
        window = self.open("test.float")
        self.assertEqual(self.changed(window, "resize grow width 40 px"), (-20, 0, 40, 0))
        self.assertEqual(self.changed(window, "resize shrink height 20"), (0, 10, 0, -20))

    def test_a_side_grows_or_shrinks_that_side(self):
        window = self.open("test.float")
        self.assertEqual(self.changed(window, "resize grow right 30 px"), (0, 0, 30, 0))
        self.assertEqual(self.changed(window, "resize grow up 10 px"), (0, -10, 0, 10))
        self.assertEqual(self.changed(window, "resize shrink left 10 px"), (10, 0, -10, 0))

    def test_ten_pixels_without_an_amount(self):
        window = self.open("test.float")
        self.assertEqual(self.changed(window, "resize grow height"), (0, -5, 0, 10))

    def test_a_floating_window_takes_the_px_amount(self):
        window = self.open("test.float")
        self.assertEqual(self.changed(window, "resize grow width 10 ppt or 40 px"),
                         (-20, 0, 40, 0))
        self.assertIn("in px", self.fails("resize grow width 10 ppt")["error"])

    def test_a_bad_resize_is_a_parse_error(self):
        self.open("test.float")
        for command in ("resize grow sideways", "resize grow width ten",
                        "resize grow width 10 px 20", "resize width 10"):
            with self.subTest(command=command):
                self.assertTrue(self.fails(command)["parse_error"])


class TiledResizeTest(ResizeTest):

    def test_a_masters_width_grows_its_column(self):
        master = self.open("test.master")
        stack = self.open("test.stack")
        self.assertEqual(self.changed(master, "resize grow width 10 ppt")[2], 128)
        # The stack's grows the stack, so the master's column shrinks
        self.assertEqual(self.changed(stack, "resize grow width 10")[2], 128)
        self.assertEqual(self.rect(master)["width"], self.rect(stack)["x"])

    def test_a_layouts_own_master_count_says_which_is_the_master(self):
        # twocol has a master even at nmaster 0
        self.command(self.connect(), "layout twocol")
        self.command(self.connect(), "nmaster 0")
        master = self.open("test.master")
        self.open("test.stack")
        self.assertGreater(self.changed(master, "resize grow width 10 ppt")[2], 0)

    def test_pixels_are_the_window_areas(self):
        master = self.open("test.master")
        self.open("test.stack")
        self.assertEqual(self.changed(master, "resize shrink width 128 px")[2], -128)

    def test_a_tile_on_a_hidden_workspace_leaves_the_shown_split(self):
        master = self.open("test.master")
        self.open("test.stack")
        hidden = [self.open(f"test.hidden{i}") for i in range(2)]
        for con_id in hidden:
            self.command(self.connect(), f"[con_id={con_id}] move to workspace other")
        before = self.rect(master)
        self.assertIn("isn't shown",
                      self.fails(f"[con_id={hidden[0]}] resize grow width 10 ppt")["error"])
        self.assertEqual(self.rect(master), before)

    def test_a_tiles_height_is_its_stacks_share(self):
        self.open("test.master")
        self.open("test.stack")
        self.assertIn("height", self.fails("resize grow height 10")["error"])


if __name__ == "__main__":
    unittest.main()
