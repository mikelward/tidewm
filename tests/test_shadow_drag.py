"""A window's shadow is where a drag resizes it, as tide's SPEC §6.4 and
§6.6 have it. A plain left press just outside a floating window, as far
out as its shadow reaches, or 8 px with shadows off, drags that edge or
corner. A tile's shadow across the master/stack edge drags that edge: the
focused tile's with shadows active, on its neighbor's side and beside it,
and both sides with shadows all. The app under a zone never sees the
press."""

import unittest

from harness import EVENT_WINDOW
from test_split import FLOAT, SplitTest

NO_BORDER = "default_border none\n"
OTHER = 'for_window [app_id="^test.other$"] floating enable\n'


class ZoneTest(SplitTest):
    """Helpers, with no tests of their own."""

    def open(self, app_id, size=None, min_size=None):
        self.wm.open_window(app_id, app_id, size=size, min_size=min_size)
        return self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                                   and e["container"]["app_id"] == app_id)["container"]["id"]

    def focus(self, con_id):
        self.command(self.connect(), f"[con_id={con_id}] focus")

    def cross(self, x, y):
        """Moves the pointer to x, y as a mouse does, which focus follows."""
        self.pointer("cross", str(x), str(y), *self.size())

    def focused(self, con_id):
        return self.windows()[con_id]["focused"]

    def assert_still(self, ids, x, y, to_x):
        before = self.boxes(ids)
        self.drag(x, y, to_x)
        self.assertEqual(self.boxes(ids), before)


class FloatingTest(ZoneTest):
    CONFIG = FLOAT + NO_BORDER

    def test_each_side_drags_alone(self):
        window = self.open("test.float", "300x200")
        x, y, w, h = self.box(window)
        self.drag(x + w + 4, y + h // 2, x + w + 4 + 50)
        self.assertEqual(self.box(window), (x, y, w + 50, h))
        self.drag(x - 4, y + h // 2, x - 4 - 30)
        self.assertEqual(self.box(window), (x - 30, y, w + 80, h))
        self.drag(x, y - 4, x, y - 4 - 20)
        self.assertEqual(self.box(window), (x - 30, y - 20, w + 80, h + 20))
        self.drag(x, y + h + 4, x, y + h + 4 - 40)
        self.assertEqual(self.box(window), (x - 30, y - 20, w + 80, h - 20))

    def test_a_corner_drags_both_its_sides(self):
        window = self.open("test.float", "300x200")
        x, y, w, h = self.box(window)
        self.drag(x + w + 3, y + h + 3, x + w + 3 + 40, y + h + 3 + 30)
        self.assertEqual(self.box(window), (x, y, w + 40, h + 30))
        self.drag(x - 3, y - 3, x - 3 - 20, y - 3 - 10)
        self.assertEqual(self.box(window), (x - 20, y - 10, w + 60, h + 40))

    def test_the_end_of_an_edge_is_its_corner(self):
        window = self.open("test.float", "300x200")
        x, y, w, h = self.box(window)
        self.drag(x + w + 3, y + h - 2, x + w + 3 + 40, y + h - 2 + 30)
        self.assertEqual(self.box(window), (x, y, w + 40, h + 30))

    def test_the_far_side_stays_put_at_the_windows_smallest(self):
        window = self.open("test.float", "300x200", min_size="100x50")
        x, y, w, h = self.box(window)
        self.drag(x - 4, y + h // 2, x + w + 100)
        self.assertEqual(self.box(window), (x + w - 100, y, 100, h))

    def test_without_shadows_the_zone_is_8_px(self):
        tile, client = self.listening("test.tile")
        window = self.open("test.float", "300x200")
        x, y, w, h = self.box(window)
        self.drag(x + w + 8, y + h // 2, x + w + 8 + 50)
        self.assertEqual(self.box(window), (x, y, w, h))
        self.assertIn(b"button\n", self.said(client))
        self.drag(x + w + 7, y + h // 2, x + w + 7 + 50)
        self.assertEqual(self.box(window), (x, y, w + 50, h))

    def test_the_window_under_the_zone_never_sees_the_press(self):
        tile, client = self.listening("test.tile")
        window = self.open("test.float", "300x200")
        x, y, w, h = self.box(window)
        self.drag(x + w + 4, y + h // 2, x + w + 4 + 50)
        self.assertEqual(self.box(window), (x, y, w + 50, h))
        self.assertNotIn(b"button\n", self.said(client))

    def test_a_press_inside_the_window_is_the_apps(self):
        window, client = self.listening("test.float")
        x, y, w, h = self.box(window)
        for px, py in [(x, y), (x + w - 1, y + h - 1), (x + w - 1, y + h // 2)]:
            self.drag(px, py, px + 50, py + 50)
            self.assertEqual(self.box(window), (x, y, w, h))
            self.assertEqual(self.said(client), [b"button\n"])

    def test_the_press_focuses_the_window(self):
        tile = self.open("test.tile")
        window = self.open("test.float", "300x200")
        self.focus(tile)
        x, y, w, h = self.box(window)
        self.drag(x + w + 4, y + h // 2, x + w + 4 + 50)
        self.assertTrue(self.windows()[window]["focused"])


class FloatingStackTest(ZoneTest):
    CONFIG = FLOAT + OTHER + NO_BORDER

    def test_the_window_on_top_takes_the_press(self):
        under = self.open("test.float", "400x300")
        over = self.open("test.other", "200x100")
        ux, uy, uw, uh = self.box(under)
        x, y, w, h = self.box(over)
        self.assertTrue(ux < x - 20 and x + w + 80 < ux + uw)  # over is within under
        self.drag(x + w + 4, y + h // 2, x + w + 4 + 50)
        self.assertEqual(self.box(over), (x, y, w + 50, h))
        self.assertEqual(self.box(under), (ux, uy, uw, uh))


class FullscreenDialogTest(ZoneTest):
    CONFIG = FLOAT + NO_BORDER

    def test_a_fullscreen_windows_dialog_over_the_zone_takes_the_press(self):
        client = self.wm.open_window("test.app", "test.app", stdin=True)
        app = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                                  and e["container"]["app_id"] == "test.app")["container"]["id"]
        window = self.open("test.float", "90x90")
        self.command(self.connect(), f"[con_id={app}] fullscreen enable")
        client.stdin.write(b"dialog\n")
        client.stdin.flush()
        dialog = self.ipc.next_event(
            EVENT_WINDOW, lambda e: e["change"] == "new"
            and e["container"]["app_id"] == "test.app.dialog")["container"]["id"]
        x, y, w, h = self.box(window)
        dx, dy, dw, dh = self.box(dialog)
        # Inside the dialog, just outside the floating window under it
        px, py = x + w + 3, y + h // 2
        self.assertTrue(dx <= px < dx + dw and dy <= py < dy + dh)
        self.drag(px, py, px + 50)
        self.assertEqual(self.box(window), (x, y, w, h))
        self.assertTrue(self.focused(dialog))
        self.assertFalse(self.focused(window))


class FloatingShadowTest(ZoneTest):
    CONFIG = "shadows floating\nshadow_blur_radius 20\n" + FLOAT + NO_BORDER

    def test_the_zone_reaches_as_far_as_the_shadow(self):
        window = self.open("test.float", "300x200")
        x, y, w, h = self.box(window)
        self.drag(x + w + 20, y + h // 2, x + w + 20 + 50)
        self.assertEqual(self.box(window), (x, y, w, h))
        self.drag(x + w + 19, y + h // 2, x + w + 19 + 50)
        self.assertEqual(self.box(window), (x, y, w + 50, h))

    def test_focus_doesnt_follow_the_pointer_into_the_zone(self):
        tile = self.open("test.tile")
        window = self.open("test.float", "300x200")
        x, y, w, h = self.box(window)
        self.pointer("move", str(x + w // 2), str(y + h // 2), *self.size())
        self.cross(x + w + 10, y + h // 2)
        self.assertTrue(self.focused(window))
        self.drag(x + w + 10, y + h // 2, x + w + 10 + 50)
        self.assertEqual(self.box(window), (x, y, w + 50, h))
        self.cross(x + w + 50 + 25, y + h // 2)  # past it, the tile's
        self.assertTrue(self.focused(tile))


class FloatingUnfocusedTest(ZoneTest):
    CONFIG = "shadows active\nshadow_blur_radius 20\n" + FLOAT + OTHER + NO_BORDER

    def test_a_window_casting_no_shadow_has_the_same_zone(self):
        window = self.open("test.float", "400x300")
        self.open("test.other", "200x100")
        self.assertFalse(self.windows()[window]["shadow"])
        x, y, w, h = self.box(window)
        self.drag(x + w + 15, y + h // 2, x + w + 15 + 50)
        self.assertEqual(self.box(window), (x, y, w + 50, h))


class FloatingShadowsOffTest(ZoneTest):
    CONFIG = "shadows disable\nshadow_blur_radius 20\n" + FLOAT + NO_BORDER

    def test_with_shadows_off_the_zone_is_8_px(self):
        window = self.open("test.float", "300x200")
        x, y, w, h = self.box(window)
        self.drag(x + w + 15, y + h // 2, x + w + 15 + 50)
        self.assertEqual(self.box(window), (x, y, w, h))
        self.drag(x + w + 7, y + h // 2, x + w + 7 + 50)
        self.assertEqual(self.box(window), (x, y, w + 50, h))


class FloatingNoRangeTest(ZoneTest):
    CONFIG = "shadows floating\nshadow_blur_radius 0\n" + FLOAT + NO_BORDER

    def test_with_a_shadow_of_no_size_the_zone_is_8_px(self):
        window = self.open("test.float", "300x200")
        x, y, w, h = self.box(window)
        self.drag(x + w + 8, y + h // 2, x + w + 8 + 50)
        self.assertEqual(self.box(window), (x, y, w, h))
        self.drag(x + w + 7, y + h // 2, x + w + 7 + 50)
        self.assertEqual(self.box(window), (x, y, w + 50, h))


class ActiveTileTest(ZoneTest):
    CONFIG = "shadows active\nshadow_blur_radius 20\n" + NO_BORDER

    def test_the_focused_masters_shadow_drags_from_the_stacks_side(self):
        x, y, w, h = self.area()
        master, stack = self.windows_in("tile", 2, 0.5)
        self.focus(master)
        edge = x + w // 2
        self.assert_still([master, stack], edge - 15, y + h // 2, edge + 100)
        self.drag(edge + 15, y + h // 2, edge + 15 + 64)
        self.assertEqual(self.box(master)[2], w // 2 + 64)

    def test_the_focused_stack_windows_shadow_drags_from_the_masters_side(self):
        x, y, w, h = self.area()
        master, stack = self.windows_in("tile", 2, 0.5)
        self.focus(stack)
        edge = x + w // 2
        self.assert_still([master, stack], edge + 15, y + h // 2, edge + 100)
        self.drag(edge - 15, y + h // 2, edge - 15 + 64)
        self.assertEqual(self.box(master)[2], w // 2 + 64)

    def test_the_shadow_reaches_no_further_than_its_range(self):
        x, y, w, h = self.area()
        master, stack = self.windows_in("tile", 2, 0.5)
        self.focus(master)
        edge = x + w // 2
        self.assert_still([master, stack], edge + 20, y + h // 2, edge + 100)
        self.focus(master)  # the press was the stack's, which took focus
        self.drag(edge + 19, y + h // 2, edge + 19 + 64)
        self.assertEqual(self.box(master)[2], w // 2 + 64)

    def test_focus_doesnt_follow_the_pointer_into_the_shadow(self):
        x, y, w, h = self.area()
        master, stack = self.windows_in("tile", 2, 0.5)
        self.focus(master)
        edge = x + w // 2
        self.pointer("move", str(edge - 100), str(y + h // 2), *self.size())
        self.cross(edge + 15, y + h // 2)  # over the stack, in the master's shadow
        self.assertTrue(self.focused(master))
        self.drag(edge + 15, y + h // 2, edge + 15 + 64)
        self.assertEqual(self.box(master)[2], w // 2 + 64)
        self.assertTrue(self.focused(master))

    def test_focus_follows_the_pointer_past_the_shadow(self):
        x, y, w, h = self.area()
        master, stack = self.windows_in("tile", 2, 0.5)
        self.focus(master)
        edge = x + w // 2
        self.pointer("move", str(edge - 100), str(y + h // 2), *self.size())
        self.cross(edge + 15, y + h // 2)
        self.assertTrue(self.focused(master))
        self.cross(edge + 25, y + h // 2)
        self.assertTrue(self.focused(stack))

    def test_only_beside_the_focused_tile(self):
        x, y, w, h = self.area()
        ids = self.windows_in("tile", 3, 0.5)
        master, upper, lower = ids
        self.focus(upper)
        edge = x + w // 2
        bottom = self.box(upper)[1] + self.box(upper)[3]
        # Beside the lower row, past the upper one's shadow
        self.assert_still(ids, edge - 15, bottom + 20, edge + 100)
        self.focus(upper)  # the press was the master's, which took focus
        self.drag(edge - 3, bottom + 20, edge - 3 + 10)  # resize_grab still drags
        self.assertEqual(self.box(master)[2], w // 2 + 10)
        # Just below the upper row, where its shadow reaches
        self.drag(edge + 10 - 15, bottom + 19, edge + 10 - 15 + 10)
        self.assertEqual(self.box(master)[2], w // 2 + 20)
        self.drag(edge + 20 - 15, y + h // 4, edge + 20 - 15 + 10)
        self.assertEqual(self.box(master)[2], w // 2 + 30)

    def test_the_window_under_the_shadow_never_sees_the_press(self):
        x, y, w, h = self.area()
        self.command(self.connect(), "layout tile")
        self.command(self.connect(), "mfact 0.5")
        master = self.open("test.master")
        stack, client = self.listening("test.stack")
        self.focus(master)
        edge = x + w // 2
        self.drag(edge + 15, y + h // 2, edge + 15 + 64)
        self.assertEqual(self.box(master)[2], w // 2 + 64)
        self.assertNotIn(b"button\n", self.said(client))

    def test_a_floating_window_focused_leaves_only_the_grab(self):
        x, y, w, h = self.area()
        self.wm.write_config(self.CONFIG + FLOAT)
        self.command(self.connect(), "reload")
        master, stack = self.windows_in("tile", 2, 0.5)
        window = self.open("test.float", "200x100")
        self.assertTrue(self.windows()[window]["focused"])
        edge = x + w // 2
        self.assert_still([master, stack], edge + 15, y + 40, edge + 100)
        self.focus(window)  # the press was the stack's, which took focus
        self.assert_still([master, stack], edge - 15, y + 40, edge + 100)
        self.focus(window)
        self.drag(edge + 3, y + 40, edge + 3 + 64)
        self.assertEqual(self.box(master)[2], w // 2 + 64)


class AllTilesTest(ZoneTest):
    CONFIG = "shadows all\nshadow_blur_radius 20\n" + NO_BORDER

    def test_every_tiles_shadow_drags_from_both_sides(self):
        x, y, w, h = self.area()
        ids = self.windows_in("tile", 3, 0.5)
        master, upper, lower = ids
        self.focus(upper)
        edge = x + w // 2
        self.drag(edge + 15, y + 3 * h // 4, edge + 15 + 10)
        self.assertEqual(self.box(master)[2], w // 2 + 10)
        self.drag(edge + 10 - 15, y + 3 * h // 4, edge + 10 - 15 + 10)
        self.assertEqual(self.box(master)[2], w // 2 + 20)
        self.assert_still(ids, edge + 20 + 20, y + h // 2, edge + 200)
        self.assert_still(ids, edge + 20 - 21, y + h // 2, edge + 200)


class FloatingOnlyTilesTest(ZoneTest):
    CONFIG = "shadows floating\nshadow_blur_radius 20\n" + NO_BORDER

    def test_tiles_that_cast_no_shadow_have_only_the_grab(self):
        x, y, w, h = self.area()
        master, stack = self.windows_in("tile", 2, 0.5)
        edge = x + w // 2
        self.assert_still([master, stack], edge + 15, y + h // 2, edge + 100)
        self.assert_still([master, stack], edge - 15, y + h // 2, edge + 100)
        self.drag(edge + 3, y + h // 2, edge + 3 + 64)
        self.assertEqual(self.box(master)[2], w // 2 + 64)


class NoRangeTilesTest(ZoneTest):
    CONFIG = "shadows all\nshadow_blur_radius 0\n" + NO_BORDER

    def test_a_shadow_of_no_size_leaves_only_the_grab(self):
        x, y, w, h = self.area()
        master, stack = self.windows_in("tile", 2, 0.5)
        edge = x + w // 2
        self.assert_still([master, stack], edge + 4, y + h // 2, edge + 100)
        self.drag(edge + 3, y + h // 2, edge + 3 + 64)
        self.assertEqual(self.box(master)[2], w // 2 + 64)


class NoGrabTilesTest(ZoneTest):
    CONFIG = "resize_grab 0\nshadows all\nshadow_blur_radius 20\n" + NO_BORDER

    def test_resize_grab_0_turns_the_shadows_drag_off_too(self):
        x, y, w, h = self.area()
        ids = self.windows_in("tile", 2, 0.5)
        edge = x + w // 2
        self.assert_still(ids, edge, y + h // 2, edge + 100)
        self.assert_still(ids, edge + 15, y + h // 2, edge + 100)


class LoneWindowTest(ZoneTest):
    CONFIG = "shadows active\nshadow_blur_radius 20\n" + NO_BORDER

    def setUp(self):
        super().setUp()
        self.command(self.connect(), "output HEADLESS-1 mode 2560x1080")

    def test_a_lone_windows_shadow_drags_its_width(self):
        x, y, w, h = self.area()
        con, = self.windows_in("tile", 1)
        self.assertEqual(self.box(con), (x + w // 10, y, w * 8 // 10, h))  # 80%
        self.assertTrue(self.windows()[con]["focused"])
        right = x + w * 9 // 10
        self.assert_still([con], right - 15, y + h // 2, right + 100)  # inside it
        self.drag(right + 15, y + h // 2, right + 15 + w // 20)
        self.assertEqual(self.box(con), (x + w // 20, y, w * 9 // 10, h))
        self.drag(x + w // 20 - 15, y + h // 2, x + w // 4 - 15)
        self.assertEqual(self.box(con), (x + w // 4, y, w // 2, h))


if __name__ == "__main__":
    unittest.main()
