"""Borders (docs/config.md): every window has a border config.h's
borderpx wide, or default_border's width, none for tide. A floating
window keeps its size, border and all, and a reload changes every
window's border at once. Its color is its class's, sway's client.CLASS
line's or config.h's: focused, focused_inactive, unfocused or urgent."""

import unittest

from harness import EVENT_WINDOW, GET_OUTPUTS, RUN_COMMAND, TidewmTest
from test_effects import BACKGROUND, EffectsHelpers

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


WIDE = "default_border pixel 4\n"
# config.h's focuscolor, bordercolor and urgentcolor
BLUE, GRAY, RED = (0x00, 0x55, 0x77), (0x44, 0x44, 0x44), (0xff, 0x00, 0x00)
GREEN, NAVY, YELLOW, PINK = (0, 0xff, 0), (0, 0, 0xff), (0xff, 0xff, 0), (0xff, 0, 0xff)
# Each class's border is its child_border, or its background without one,
# as client.unfocused's here; the other colors are for what tidewm lacks
FOCUSED = "client.focused #111111 #aa0000 #ffffff #121212 #00ff00\n"
UNFOCUSED = "client.unfocused #111111 #0000ff #ffffff\n"
INACTIVE = "client.focused_inactive #111111 #131313 #ffffff #141414 #ffff00\n"
URGENT = "client.urgent #111111 #151515 #ffffff #161616 #ff00ff\n"
COLORS = WIDE + FOCUSED + UNFOCUSED + INACTIVE + URGENT


class ColorHelpers(EffectsHelpers):
    """Helpers, with no tests of their own: two tiles, the second focused."""

    def setUp(self):
        super().setUp()
        self.first = self.open("test.first")
        self.second = self.open("test.second")

    def color(self, con_id):
        """The color of the window's border, from inside its left one."""
        x, y, _, h = self.rect(con_id)
        return self.pixels((x + 1, y + h // 2))[0]

    def colors(self):
        return self.color(self.first), self.color(self.second)

    def focus(self, con_id):
        self.command(self.connect(), f"[con_id={con_id}] focus")

    def urgent(self, con_id, how):
        self.command(self.connect(), f"[con_id={con_id}] urgent {how}")

    def reload(self, config):
        self.wm.write_config(config)
        return self.connect().request(RUN_COMMAND, "reload")[0]


class BorderColorTest(ColorHelpers):
    CONFIG = COLORS

    def test_the_focused_window_and_the_other_have_their_classes_colors(self):
        self.assertEqual(self.colors(), (NAVY, GREEN))

    def test_the_colors_follow_focus(self):
        self.focus(self.first)
        self.assertEqual(self.colors(), (GREEN, NAVY))

    def test_an_urgent_window_has_urgents_until_its_focused(self):
        self.urgent(self.first, "enable")
        self.assertEqual(self.colors(), (PINK, GREEN))
        self.focus(self.first)
        self.assertEqual(self.colors(), (GREEN, NAVY))

    def test_urgent_wins_over_focused(self):
        self.urgent(self.second, "enable")
        self.assertEqual(self.colors(), (NAVY, PINK))
        self.urgent(self.second, "disable")
        self.assertEqual(self.colors(), (NAVY, GREEN))

    def test_a_reload_changes_the_colors_of_the_windows_there_are(self):
        self.urgent(self.first, "enable")
        third = self.open("test.third")
        self.assertTrue(self.reload(
            WIDE + "client.focused #000000 #ff00ff #000000\n"
            "client.unfocused #000000 #00ff00 #000000\n"
            "client.urgent #000000 #000000 #000000 #000000 #ffff00\n")["success"])
        self.assertEqual((*self.colors(), self.color(third)), (YELLOW, GREEN, PINK))

    def test_a_reload_without_the_lines_brings_config_hs_back(self):
        self.urgent(self.first, "enable")
        third = self.open("test.third")
        self.assertTrue(self.reload(WIDE)["success"])
        self.assertEqual((*self.colors(), self.color(third)), (RED, GRAY, BLUE))

    def test_a_reload_that_fails_leaves_the_colors(self):
        self.assertFalse(self.reload(WIDE + "client.focused #ff0000\n")["success"])
        self.assertEqual(self.colors(), (NAVY, GREEN))

    def test_a_translucent_color_shows_what_is_under_it(self):
        self.assertTrue(self.reload(
            WIDE + "client.focused #000000 #000000 #000000 #000000 #ff000080\n")["success"])
        # Half red, over config.h's rootcolor
        want = (0x80 + BACKGROUND[0] // 2, BACKGROUND[1] // 2, BACKGROUND[2] // 2)
        got = self.color(self.second)
        self.assertTrue(all(abs(g - w) <= 2 for g, w in zip(got, want)), f"{got}, not {want}")


class DefaultColorTest(ColorHelpers):
    CONFIG = WIDE

    def test_without_the_lines_the_colors_are_config_hs(self):
        self.assertEqual(self.colors(), (GRAY, BLUE))
        self.urgent(self.first, "enable")
        self.assertEqual(self.colors(), (RED, BLUE))

    def test_a_class_without_a_line_keeps_config_hs(self):
        self.assertTrue(self.reload(WIDE + UNFOCUSED)["success"])
        self.assertEqual(self.colors(), (NAVY, BLUE))


class InactiveColorTest(ColorHelpers):
    """Two monitors: tests/capture sees the first, where the two tiles are."""
    CONFIG = COLORS
    OUTPUTS = 2

    def setUp(self):
        TidewmTest.setUp(self)
        self.ipc = self.connect()
        self.ipc.subscribe("window")
        left, right = sorted(self.connect().request(GET_OUTPUTS), key=lambda o: o["rect"]["x"])
        self.assertEqual(left["rect"]["x"], 0)
        self.left, self.right = left["name"], right["name"]
        self.command(self.connect(), f"focus output {self.left}")
        self.first = self.open("test.first")
        self.second = self.open("test.second")
        self.command(self.connect(), f"focus output {self.right}")

    def test_the_window_focus_would_come_back_to_is_focused_inactive(self):
        self.open("test.there")
        self.assertEqual(self.colors(), (NAVY, YELLOW))
        self.command(self.connect(), f"focus output {self.left}")
        self.assertEqual(self.colors(), (NAVY, GREEN))

    def test_urgent_wins_over_focused_inactive(self):
        self.open("test.there")
        self.urgent(self.second, "enable")
        self.assertEqual(self.colors(), (NAVY, PINK))

    def test_without_its_line_focused_inactive_is_as_unfocused(self):
        self.open("test.there")
        self.assertTrue(self.reload(WIDE + FOCUSED + UNFOCUSED)["success"])
        self.assertEqual(self.colors(), (NAVY, NAVY))
        self.assertTrue(self.reload(WIDE)["success"])
        self.assertEqual(self.colors(), (GRAY, GRAY))


class ColorLinesTest(ColorHelpers):
    CONFIG = WIDE

    def fails(self, config):
        result = self.reload(config)
        self.assertFalse(result["success"], result)
        return result["error"]

    def test_every_line_sway_takes_loads(self):
        self.assertTrue(self.reload(
            WIDE + "client.focused #4c7899 #285577 #ffffff #2e9ef4 #285577\n"
            "client.focused_inactive #333333 #5f676a #ffffff #484e50\n"
            "client.unfocused 333333 222244ff 888888\n"
            "client.urgent #2f343a #900000 #ffffff #900000 #90000080\n"
            "client.focused_tab_title #333333 #5f676a #ffffff\n"
            "client.placeholder #000000 #0c0c0c #ffffff #000000 #0c0c0c\n"
            "client.background #ffffff\n")["success"])
        # The fourth color given is the indicator, not the border
        self.assertEqual(self.colors(), ((0x22, 0x22, 0x44), (0x28, 0x55, 0x77)))

    def test_too_few_or_too_many_colors_fail_naming_the_line(self):
        for line in ("client.focused", "client.unfocused #111111 #222222",
                     "client.urgent #111111 #222222 #333333 #444444 #555555 #666666",
                     "client.focused_tab_title #111111"):
            name = line.split()[0]
            self.assertIn(f":2: {name} takes 3 to 5 colors", self.fails(WIDE + line + "\n"))

    def test_what_isnt_a_color_fails_naming_it(self):
        for bad in ("#12345", "#1234567", "#gggggg", "red", "#"):
            for line in (f"client.focused {bad} #222222 #333333",
                         f"client.focused_inactive #111111 #222222 {bad}",
                         f"client.urgent #111111 #222222 #333333 {bad}",
                         f"client.unfocused #111111 #222222 #333333 #444444 {bad}"):
                name = line.split()[0]
                self.assertIn(f":2: {name}: {bad} isn't a color", self.fails(WIDE + line + "\n"))

    def test_a_class_sway_hasnt_is_no_setting(self):
        self.assertIn("no setting is called client.active",
                      self.fails("client.active #111111 #222222 #333333\n"))

    def test_it_cant_be_in_a_mode(self):
        self.assertIn("client.focused can't be in a mode",
                      self.fails("mode resize {\n    " + FOCUSED + "}\n"))


if __name__ == "__main__":
    unittest.main()
