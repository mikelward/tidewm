"""Effects (effects.h, docs/config.md): a window fades in as it opens, and
the dim fades as focus moves, each over fade_duration, at once without
it; shadows says which windows cast a soft shadow, each floating one, the
focused one, or all, shadow_blur_radius how far it reaches, and
shadow_color its color, black at 0x44 without the line. A fade_duration of an hour keeps a fade near its start for as long as a test
looks."""

import os
import subprocess
import unittest

from harness import EVENT_WINDOW, GET_CONFIG, GET_OUTPUTS, ROOT, RUN_COMMAND, TIMEOUT, TidewmTest

CAPTURE = os.path.join(ROOT, "tests", "capture")
FLOAT = 'for_window [app_id="^test.float$"] floating enable\n'
HOUR = "fade_duration 3600000\n"
BACKGROUND = (0x22, 0x22, 0x22)  # config.h's rootcolor
WINDOW = (0x33, 0x66, 0x99)  # tests/client fills its window with it
DIMMED = tuple(round(c * 0.5) for c in WINDOW)  # under dim_inactive 0.5


def nearer(color, a, b):
    """Whether color is nearer a than b."""
    def distance(x, y):
        return sum((i - j) ** 2 for i, j in zip(x, y))
    return distance(color, a) < distance(color, b)


class EffectsHelpers(TidewmTest):
    """Helpers, with no tests of their own."""

    def setUp(self):
        super().setUp()
        self.ipc = self.connect()
        self.ipc.subscribe("window")

    def open(self, app_id, **kwargs):
        self.wm.open_window(app_id, app_id, **kwargs)
        return self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                                   and e["container"]["app_id"] == app_id)["container"]["id"]

    def pixels(self, *points):
        """The output's color at each (x, y), from tests/capture's frame."""
        out = subprocess.run([CAPTURE, "pixels", *(f"{x},{y}" for x, y in points)],
                             env=self.wm.env, check=True, capture_output=True,
                             timeout=TIMEOUT).stdout.decode().split()
        self.assertNotIn("none", out, "a pixel off the output")
        return [tuple(int(p[i:i + 2], 16) for i in (0, 2, 4)) for p in out]

    def rect(self, con_id):
        r = self.windows()[con_id]["rect"]
        return r["x"], r["y"], r["width"], r["height"]

    def middle(self, con_id):
        x, y, w, h = self.rect(con_id)
        return x + w // 2, y + h // 2


class FadeTest(EffectsHelpers):
    CONFIG = HOUR + "dim_inactive 0.5\n"

    def test_a_window_fades_in_as_it_opens(self):
        window = self.open("test.app")
        color, = self.pixels(self.middle(window))
        self.assertTrue(nearer(color, BACKGROUND, WINDOW), f"{color} already shows")

    def test_the_dim_fades_as_focus_moves(self):
        # Both shown in full first, as their own fades in would hide the dim's
        self.wm.write_config("dim_inactive 0.5\n")
        self.reload()
        first = self.open("test.first")
        second = self.open("test.second")
        self.command(self.connect(), f"[con_id={first}] focus")
        self.wm.write_config(self.CONFIG)
        self.reload()
        self.command(self.connect(), f"[con_id={second}] focus")
        self.assertTrue(self.windows()[first]["dimmed"], "it's fading to dimmed")
        color, = self.pixels(self.middle(first))
        self.assertTrue(nearer(color, WINDOW, DIMMED), f"{color} is dimmed already")

    def reload(self):
        result, = self.connect().request(RUN_COMMAND, "reload")
        self.assertTrue(result["success"], result)

    def test_a_window_that_opens_dimmed_fades_in_dim_and_all(self):
        # The focus guard holds the second from focus, so it opens dimmed
        self.wm.write_config("focus_guard enable\ndim_inactive 0.5\n")
        self.reload()
        self.open("test.first")
        self.wm.write_config("focus_guard enable\n" + self.CONFIG)
        self.reload()
        held = self.open("test.held")
        self.assertTrue(self.windows()[held]["dimmed"])
        color, = self.pixels(self.middle(held))
        dark = tuple(round(c * 0.5) for c in BACKGROUND)
        self.assertTrue(nearer(color, BACKGROUND, dark), f"{color}: its dim shows already")


class InstantTest(EffectsHelpers):
    CONFIG = "dim_inactive 0.5\n"

    def test_without_fade_duration_a_window_shows_as_it_opens(self):
        window = self.open("test.app")
        self.assertEqual(self.pixels(self.middle(window)), [WINDOW])

    def test_without_fade_duration_the_dim_comes_at_once(self):
        first = self.open("test.first")
        self.open("test.second")
        color, = self.pixels(self.middle(first))
        self.assertTrue(nearer(color, DIMMED, WINDOW), f"{color} isn't dimmed")


class ShadowTest(EffectsHelpers):
    CONFIG = "shadows enable\n" + FLOAT

    def test_a_floating_window_casts_a_shadow(self):
        window = self.open("test.float", size="300x200")
        x, y, w, h = self.rect(window)
        near, far = self.pixels((x - 2, y + h // 2), (x - 20, y + h // 2))
        self.assertTrue(all(c < b for c, b in zip(near, BACKGROUND)), f"{near} isn't shadowed")
        self.assertEqual(far, BACKGROUND, "the shadow ends within its range")

    def test_a_tiled_window_casts_none(self):
        first = self.open("test.first")
        second = self.open("test.second")
        x, y, w, h = self.rect(first)
        self.assertGreaterEqual(self.rect(second)[0], x + w, "the second is beside it")
        # Inside the first, where the second's shadow would fall over it
        self.assertEqual(self.pixels((x + w - 3, y + h // 2)), [WINDOW])

    def test_in_the_floating_layout_floating_gives_and_takes_the_shadow(self):
        window = self.open("test.float", size="300x200")
        self.command(self.connect(), "layout floating")
        x, y, w, h = self.rect(window)
        self.command(self.connect(), f"[con_id={window}] floating disable")
        self.assertEqual(self.pixels((x - 2, y + h // 2)), [BACKGROUND], "tiled, it casts none")
        self.command(self.connect(), f"[con_id={window}] floating enable")
        near, = self.pixels((x - 2, y + h // 2))
        self.assertTrue(all(c < b for c, b in zip(near, BACKGROUND)), f"{near} isn't shadowed")

    def test_a_reload_without_shadows_takes_them_away(self):
        window = self.open("test.float", size="300x200")
        self.wm.write_config(FLOAT)
        result, = self.connect().request(RUN_COMMAND, "reload")
        self.assertTrue(result["success"], result)
        x, y, w, h = self.rect(window)
        self.assertEqual(self.pixels((x - 2, y + h // 2)), [BACKGROUND])


class ShadowColorTest(EffectsHelpers):
    CONFIG = "shadows enable\n" + FLOAT

    def reload(self, lines):
        self.wm.write_config(lines + "shadows enable\n" + FLOAT)
        result, = self.connect().request(RUN_COMMAND, "reload")
        self.assertTrue(result["success"], result)

    def near(self, window):
        x, y, w, h = self.rect(window)
        color, = self.pixels((x - 2, y + h // 2))
        return color

    def casts(self, window):
        return self.windows()[window]["shadow"]

    def test_a_reload_with_a_new_color_redraws_the_shadow(self):
        window = self.open("test.float", size="300x200")
        black = self.near(window)
        self.assertTrue(all(c < b for c, b in zip(black, BACKGROUND)), f"{black} isn't shadowed")
        self.reload("shadow_color #ffffff80\n")
        glow = self.near(window)
        self.assertTrue(all(c > b for c, b in zip(glow, BACKGROUND)), f"{glow} isn't lit")
        self.reload("shadow_color #ff0000\n")
        red = self.near(window)
        self.assertGreater(red[0], BACKGROUND[0], f"{red} isn't red")
        self.assertLess(red[1], BACKGROUND[1], f"{red} isn't red")
        self.reload("shadow_color #00000044\n")
        self.assertEqual(self.near(window), black, "#00000044 is the default")
        self.reload("")
        self.assertEqual(self.near(window), black, "without the line, the default")

    def test_a_color_with_no_alpha_casts_none(self):
        window = self.open("test.float", size="300x200")
        self.reload("shadow_color #ffffff00\n")
        self.assertFalse(self.casts(window))
        self.assertEqual(self.near(window), BACKGROUND)
        self.reload("shadow_color #ffffff01\n")
        self.assertTrue(self.casts(window))


class BadShadowColorAtStartTest(EffectsHelpers):
    CONFIG = "shadow_color glow\nshadows enable\n" + FLOAT

    def test_tidewm_starts_without_the_bad_line_and_says_why(self):
        config = self.connect().request(GET_CONFIG)
        self.assertEqual(config["errors"], [f"{self.wm.config_path}:1: shadow_color takes "
                                            "a color as #RRGGBB or #RRGGBBAA"])
        window = self.open("test.float", size="300x200")
        x, y, w, h = self.rect(window)
        near, = self.pixels((x - 2, y + h // 2))
        self.assertTrue(all(c < b for c, b in zip(near, BACKGROUND)),
                        f"{near} isn't shadowed in the default color")


class NoShadowTest(EffectsHelpers):
    CONFIG = FLOAT

    def test_without_shadows_a_floating_window_casts_none(self):
        window = self.open("test.float", size="300x200")
        x, y, w, h = self.rect(window)
        self.assertEqual(self.pixels((x - 2, y + h // 2)), [BACKGROUND])


class EffectsConfigTest(EffectsHelpers):
    CONFIG = ""

    def fails(self, line):
        self.wm.write_config(line)
        result, = self.connect().request(RUN_COMMAND, "reload")
        self.assertFalse(result["success"], f"{line!r}: {result}")
        return result["error"]

    def test_bad_lines_say_what_they_take(self):
        self.assertIn("fade_duration takes milliseconds", self.fails("fade_duration -1\n"))
        self.assertIn("fade_duration takes milliseconds", self.fails("fade_duration 3600001\n"))
        self.assertIn("fade_duration takes milliseconds", self.fails("fade_duration soon\n"))
        for line in ("shadows on\n", "shadows\n", "shadows all floating\n"):
            self.assertIn("shadows takes disable, floating, active or all", self.fails(line))
        for line in ("shadow_blur_radius -1\n", "shadow_blur_radius 101\n",
                     "shadow_blur_radius wide\n", "shadow_blur_radius\n",
                     "shadow_blur_radius 8 9\n"):
            self.assertIn("shadow_blur_radius takes pixels, from 0 to 100", self.fails(line))
        for line in ("shadow_color\n", "shadow_color glow\n", "shadow_color #fff\n",
                     "shadow_color #1234567\n", "shadow_color #12345678 #12345678\n",
                     "shadow_color #gggggg\n", "shadow_color ##123456\n"):
            self.assertIn("shadow_color takes a color as #RRGGBB or #RRGGBBAA", self.fails(line))

    def test_a_shadow_line_cant_be_in_a_mode(self):
        self.assertIn("shadows can't be in a mode", self.fails("mode m {\nshadows all\n}\n"))
        self.assertIn("shadow_blur_radius can't be in a mode",
                      self.fails("mode m {\nshadow_blur_radius 8\n}\n"))
        self.assertIn("shadow_color can't be in a mode",
                      self.fails("mode m {\nshadow_color #000000\n}\n"))


NO_BORDER = "default_border none\n"  # so a window's edge is its own color


class ShadowHelpers(EffectsHelpers):
    """Helpers, with no tests of their own."""

    def configure(self, lines):
        self.wm.write_config(lines + NO_BORDER + FLOAT)
        result, = self.connect().request(RUN_COMMAND, "reload")
        self.assertTrue(result["success"], result)

    def casting(self):
        """The app_ids of the windows casting a shadow."""
        return {w["app_id"] for w in self.windows().values() if w["shadow"]}

    def focus(self, con_id):
        self.command(self.connect(), f"[con_id={con_id}] focus")

    def shadowed(self, point, under):
        """Whether the pixel at point is darker than under, what's there."""
        color, = self.pixels(point)
        self.assertTrue(all(c <= u for c, u in zip(color, under)), f"{color} at {point}")
        return color != under


class ShadowModesTest(ShadowHelpers):
    """Two tiles and a floating window over them."""

    CONFIG = "shadows all\n" + NO_BORDER + FLOAT

    def setUp(self):
        super().setUp()
        self.first = self.open("test.first")
        self.second = self.open("test.second")
        self.float = self.open("test.float", size="300x200")

    def test_all_is_every_window_whichever_is_focused(self):
        everything = {"test.first", "test.second", "test.float"}
        self.assertEqual(self.casting(), everything)
        self.focus(self.first)
        self.assertEqual(self.casting(), everything)

    def test_floating_is_each_floating_window_focused_or_not(self):
        self.configure("shadows floating\n")
        self.assertEqual(self.casting(), {"test.float"})
        self.focus(self.first)
        self.assertEqual(self.casting(), {"test.float"})

    def test_enable_means_floating(self):
        self.configure("shadows enable\n")
        self.focus(self.first)
        self.assertEqual(self.casting(), {"test.float"})

    def test_disable_is_none(self):
        self.configure("shadows disable\n")
        self.assertEqual(self.casting(), set())

    def test_without_the_line_none_casts_one(self):
        self.configure("")
        self.assertEqual(self.casting(), set())

    def test_active_is_the_focused_window_and_follows_focus(self):
        self.configure("shadows active\n")
        self.assertEqual(self.casting(), {"test.float"})
        self.focus(self.first)
        self.assertEqual(self.casting(), {"test.first"})
        self.focus(self.second)
        self.assertEqual(self.casting(), {"test.second"})
        self.focus(self.float)
        self.assertEqual(self.casting(), {"test.float"})

    def test_a_focus_event_says_the_window_casts_one(self):
        self.configure("shadows active\n")
        self.focus(self.first)
        event = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "focus"
                                    and e["container"]["id"] == self.first)
        self.assertTrue(event["container"]["shadow"])

    def test_with_no_window_focused_active_casts_none(self):
        self.configure("shadows active\n")
        here = self.windows()[self.first]["workspace"]
        self.command(self.connect(), "workspace empty")
        self.assertEqual(self.casting(), set())
        self.command(self.connect(), f"workspace {here}")
        self.assertEqual(len(self.casting()), 1)
        focused, = [w["app_id"] for w in self.windows().values() if w["focused"]]
        self.assertEqual(self.casting(), {focused})

    def test_a_window_on_a_workspace_not_shown_casts_none(self):
        self.command(self.connect(), "workspace empty")
        self.assertEqual(self.casting(), set())

    def test_a_fullscreen_window_casts_none(self):
        for window, app_id in ((self.first, "test.first"), (self.float, "test.float")):
            for mode in ("all", "active"):
                with self.subTest(window=app_id, shadows=mode):
                    self.configure(f"shadows {mode}\n")
                    self.focus(window)
                    self.assertIn(app_id, self.casting())
                    self.command(self.connect(), f"[con_id={window}] fullscreen enable")
                    self.assertNotIn(app_id, self.casting())
                    self.command(self.connect(), f"[con_id={window}] fullscreen disable")
                    self.assertIn(app_id, self.casting())

    def test_a_maximized_window_casts_none(self):
        for window, app_id in ((self.first, "test.first"), (self.float, "test.float")):
            for mode in ("all", "active", "floating"):
                with self.subTest(window=app_id, shadows=mode):
                    self.configure(f"shadows {mode}\n")
                    self.focus(window)
                    self.command(self.connect(), f"[con_id={window}] maximize enable")
                    self.assertNotIn(app_id, self.casting())
                    self.command(self.connect(), f"[con_id={window}] maximize disable")
                    self.assertEqual(app_id in self.casting(),
                                     mode != "floating" or window == self.float)

    def test_a_reload_changes_which_windows_cast_one(self):
        self.focus(self.second)
        for mode, casting in (("active", {"test.second"}), ("floating", {"test.float"}),
                              ("all", {"test.first", "test.second", "test.float"}),
                              ("disable", set())):
            self.configure(f"shadows {mode}\n")
            self.assertEqual(self.casting(), casting, mode)

    def test_a_radius_of_0_casts_none(self):
        self.configure("shadows all\nshadow_blur_radius 0\n")
        self.assertEqual(self.casting(), set())
        self.configure("shadows all\nshadow_blur_radius 1\n")
        self.assertEqual(self.casting(), {"test.first", "test.second", "test.float"})

    def test_a_bad_reload_leaves_the_shadows_as_they_were(self):
        self.wm.write_config("shadows none\n")
        result, = self.connect().request(RUN_COMMAND, "reload")
        self.assertFalse(result["success"], result)
        self.assertEqual(self.casting(), {"test.first", "test.second", "test.float"})


class TileShadowTest(ShadowHelpers):
    """Two tiles side by side: tiles touch, so one's shadow can only fall
    on the other."""

    CONFIG = "shadows active\n" + NO_BORDER

    def setUp(self):
        super().setUp()
        self.first = self.open("test.first")
        self.second = self.open("test.second")
        x, y, w, h = self.rect(self.first)
        self.assertEqual(self.rect(self.second)[0], x + w, "the second is beside it")
        # Just inside each, either side of where they meet
        self.in_first, self.in_second = (x + w - 2, y + h // 2), (x + w + 1, y + h // 2)

    def falls_on(self):
        """Which of the two a shadow falls on."""
        return {name for name, point in (("first", self.in_first), ("second", self.in_second))
                if self.shadowed(point, WINDOW)}

    def test_the_focused_tiles_shadow_lies_over_its_neighbor(self):
        self.assertEqual(self.falls_on(), {"first"})
        self.focus(self.first)
        self.assertEqual(self.falls_on(), {"second"})

    def test_with_all_only_the_focused_tiles_lies_over_its_neighbor(self):
        self.configure("shadows all\n")
        self.assertEqual(self.casting(), {"test.first", "test.second"})
        self.assertEqual(self.falls_on(), {"first"})
        self.focus(self.first)
        self.assertEqual(self.falls_on(), {"second"})

    def test_with_floating_a_tile_casts_none(self):
        self.configure("shadows floating\n")
        self.assertEqual(self.falls_on(), set())

    def test_a_tiles_shadow_reaches_the_radius(self):
        x, y = self.in_first
        self.assertFalse(self.shadowed((x - 10, y), WINDOW), "past 8 pixels")
        self.configure("shadows active\nshadow_blur_radius 20\n")
        self.assertTrue(self.shadowed((x - 10, y), WINDOW))
        self.assertFalse(self.shadowed((x - 20, y), WINDOW), "past 20 pixels")

    def test_a_click_goes_through_a_tiles_shadow_away_from_the_edge(self):
        # A third tile, under the second: its shadow is over the second,
        # where there's no edge to drag, and a click there is the second's.
        # A press on a shadow across the master/stack edge drags the edge
        # (test_shadow_drag.py).
        self.configure("shadows active\nshadow_blur_radius 100\n")
        third = self.open("test.third")
        tx, ty, tw, th = self.rect(third)
        self.assertEqual(self.rect(self.second)[1] + self.rect(self.second)[3], ty,
                         "the third is under the second")
        x, y = tx + tw // 2, ty - 50
        self.assertTrue(self.shadowed((x, y), WINDOW))
        area = self.connect().request(GET_OUTPUTS)[0]["rect"]
        subprocess.run([os.path.join(ROOT, "tests", "pointer"), "move", str(x), str(y),
                        str(area["width"]), str(area["height"]), "click"],
                       env=self.wm.env, check=True, timeout=TIMEOUT)
        self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "focus"
                            and e["container"]["id"] == self.second)
        self.assertEqual(self.casting(), {"test.second"})


class WallpaperShadowTest(ShadowHelpers):
    """A lone tile at half the width, the wallpaper either side, with a
    floating window over it."""

    CONFIG = "single_window 1.5 0.5\nshadows all\n" + NO_BORDER + FLOAT

    def setUp(self):
        super().setUp()
        self.lone = self.open("test.lone")
        self.float = self.open("test.float", size="300x200")
        x, y, w, h = self.rect(self.lone)
        self.assertGreater(x, 20, "the wallpaper shows beside it")
        self.beside = (x - 2, y + 10)

    def test_with_all_a_tile_not_focused_shadows_the_wallpaper(self):
        self.assertTrue(self.shadowed(self.beside, BACKGROUND))
        self.focus(self.lone)
        self.assertTrue(self.shadowed(self.beside, BACKGROUND))

    def test_with_active_only_the_focused_one_does(self):
        self.configure("single_window 1.5 0.5\nshadows active\n")
        self.assertFalse(self.shadowed(self.beside, BACKGROUND))
        self.focus(self.lone)
        self.assertTrue(self.shadowed(self.beside, BACKGROUND))

    def test_in_the_floating_layout_a_window_not_floating_casts_one_too(self):
        self.command(self.connect(), "layout floating")
        self.assertEqual(self.casting(), {"test.lone", "test.float"})
        self.assertTrue(self.shadowed(self.beside, BACKGROUND))


class FloatingShadowTest(ShadowHelpers):
    """A floating window alone, over the wallpaper."""

    CONFIG = "shadows floating\nshadow_blur_radius 4\n" + NO_BORDER + FLOAT

    def setUp(self):
        super().setUp()
        x, y, w, h = self.rect(self.open("test.float", size="300x200"))
        self.near, self.far = (x - 2, y + h // 2), (x - 5, y + h // 2)

    def test_the_radius_is_how_far_the_shadow_reaches(self):
        self.assertTrue(self.shadowed(self.near, BACKGROUND))
        self.assertFalse(self.shadowed(self.far, BACKGROUND), "past 4 pixels")

    def test_a_reload_changes_the_radius(self):
        self.configure("shadows floating\nshadow_blur_radius 20\n")
        self.assertTrue(self.shadowed(self.far, BACKGROUND))
        self.configure("shadows floating\n")
        self.assertTrue(self.shadowed(self.far, BACKGROUND), "8 pixels without the line")
        self.configure("shadows floating\nshadow_blur_radius 0\n")
        self.assertFalse(self.shadowed(self.near, BACKGROUND))


class ShadowOutputsTest(ShadowHelpers):
    CONFIG = "shadows active\n" + NO_BORDER
    OUTPUTS = 2

    def outputs(self):
        """The first output, which tests/capture sees, and the one right of it."""
        left, right = sorted(self.connect().request(GET_OUTPUTS), key=lambda o: o["rect"]["x"])
        self.assertEqual(left["rect"]["x"], 0)
        return left, right

    def test_the_shadow_follows_focus_to_another_monitor(self):
        left, right = self.outputs()
        self.command(self.connect(), f"focus output {left['name']}")
        self.open("test.here")
        self.command(self.connect(), f"focus output {right['name']}")
        self.assertEqual(self.casting(), set(), "an empty monitor has the focus")
        self.open("test.there")
        self.assertEqual(self.casting(), {"test.there"})
        self.command(self.connect(), f"focus output {left['name']}")
        self.assertEqual(self.casting(), {"test.here"})

    def test_a_tiles_shadow_stays_on_its_monitor(self):
        left, right = self.outputs()
        self.command(self.connect(), f"focus output {right['name']}")
        there = self.open("test.there")
        x, y, w, h = self.rect(there)
        self.assertEqual(x, left["rect"]["width"], "it's at its monitor's left edge")
        self.assertEqual(self.casting(), {"test.there"})
        self.assertFalse(self.shadowed((x - 2, y + h // 2), BACKGROUND))


if __name__ == "__main__":
    unittest.main()
