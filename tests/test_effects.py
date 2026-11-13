"""Effects (effects.h, docs/config.md): a window fades in as it opens, and
the dim fades as focus moves, each over fade_duration, at once without
it; with shadows enable, a floating window casts a soft shadow, and a
tiled one none. A fade_duration of an hour keeps a fade near its start for
as long as a test looks."""

import os
import subprocess
import unittest

from harness import EVENT_WINDOW, ROOT, RUN_COMMAND, TIMEOUT, TidewmTest

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
        self.assertIn("shadows takes enable or disable", self.fails("shadows on\n"))


if __name__ == "__main__":
    unittest.main()
