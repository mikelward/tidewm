"""Borders (docs/config.md): every window has a border config.h's
borderpx wide, or default_border's width, none for tide. A floating
window keeps its size, border and all, and a reload changes every
window's border at once."""

import unittest

from harness import EVENT_WINDOW, RUN_COMMAND, TidewmTest

FLOAT = 'for_window [app_id="^test.float$"] floating enable\n'


class BorderHelpers(TidewmTest):
    """Helpers, with no tests of their own."""
    CONFIG = FLOAT

    def setUp(self):
        super().setUp()
        self.ipc = self.connect()
        self.ipc.subscribe("window")

    def open(self, app_id, **kwargs):
        self.wm.open_window(app_id, app_id, **kwargs)
        return self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                                   and e["container"]["app_id"] == app_id)["container"]["id"]

    def border(self, con_id):
        window = self.windows()[con_id]
        return window["border"], window["current_border_width"]

    def size(self, con_id):
        rect = self.windows()[con_id]["rect"]
        return rect["width"], rect["height"]

    def reload(self, config):
        self.wm.write_config(config)
        result, = self.connect().request(RUN_COMMAND, "reload")
        self.assertTrue(result["success"], result)
        return result


class DefaultBorderTest(BorderHelpers):
    def test_without_the_line_a_window_has_config_hs_border(self):
        window = self.open("test.tiled")
        self.assertEqual(self.border(window), ("pixel", 1))

    def test_a_reload_with_none_takes_every_border_away(self):
        tiled = self.open("test.tiled")
        floating = self.open("test.float", size="300x200")
        self.assertEqual(self.size(floating), (302, 202))
        self.reload("default_border none\n" + FLOAT)
        self.assertEqual([self.border(tiled), self.border(floating)],
                         [("none", 0), ("none", 0)])
        # Its place and size stay, border and all
        self.assertEqual(self.size(floating), (302, 202))


class NoBorderTest(BorderHelpers):
    CONFIG = "default_border none\n" + FLOAT

    def test_a_window_opens_with_no_border(self):
        window = self.open("test.tiled")
        self.assertEqual(self.border(window), ("none", 0))

    def test_a_floating_window_is_the_size_it_asks(self):
        window = self.open("test.float", size="300x200")
        self.assertEqual((self.border(window), self.size(window)), (("none", 0), (300, 200)))

    def test_a_window_out_of_fullscreen_still_has_none(self):
        window = self.open("test.tiled")
        self.command(self.connect(), f"[con_id={window}] fullscreen enable")
        self.command(self.connect(), f"[con_id={window}] fullscreen disable")
        self.assertEqual(self.border(window), ("none", 0))


class PixelBorderTest(BorderHelpers):
    CONFIG = "default_border pixel 3\n" + FLOAT

    def test_a_floating_window_has_the_width_round_it(self):
        window = self.open("test.float", size="300x200")
        self.assertEqual((self.border(window), self.size(window)), (("pixel", 3), (306, 206)))

    def test_a_fullscreen_window_has_none(self):
        window = self.open("test.tiled")
        self.command(self.connect(), f"[con_id={window}] fullscreen enable")
        self.assertEqual(self.border(window), ("none", 0))

    def test_pixel_alone_is_config_hs_width(self):
        window = self.open("test.tiled")
        self.reload("default_border pixel\n" + FLOAT)
        self.assertEqual(self.border(window), ("pixel", 1))


class BadBorderTest(BorderHelpers):
    def fails(self, line):
        self.wm.write_config(line + "\n")
        result, = self.connect().request(RUN_COMMAND, "reload")
        self.assertFalse(result["success"], result)
        return result["error"]

    def test_normal_fails_naming_the_setting(self):
        # tidewm has no title bars
        self.assertIn("default_border takes none, or pixel", self.fails("default_border normal"))

    def test_a_width_past_100_fails(self):
        self.assertIn("default_border takes none, or pixel", self.fails("default_border pixel 101"))

    def test_none_with_a_width_fails(self):
        self.assertIn("default_border takes none, or pixel", self.fails("default_border none 2"))

    def test_it_cant_be_in_a_mode(self):
        self.assertIn("default_border can't be in a mode",
                      self.fails("mode resize {\n    default_border none\n}"))


if __name__ == "__main__":
    unittest.main()
