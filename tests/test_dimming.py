"""Dimming (tide's SPEC §6.2): every window but the focused one is dimmed,
at the config file's dim_inactive strength. A monitor showing one window,
or a fullscreen one, has nothing to tell apart, so it doesn't dim; nor
does a window that would look broken dimmed: picture-in-picture, video,
and one a rule or the dim command exempts. Clicks go through the dim."""

import os
import subprocess
import unittest

from harness import (EVENT_WINDOW, GET_CONFIG, GET_OUTPUTS, GET_SEATS, ROOT, RUN_COMMAND,
                     TIMEOUT, TidewmTest)

POINTER = os.path.join(ROOT, "tests", "pointer")


class DimHelpers(TidewmTest):
    """Helpers, with no tests of their own."""
    CONFIG = "dim_inactive 0.07\n"

    def setUp(self):
        super().setUp()
        self.ipc = self.connect()
        self.ipc.subscribe("window")

    def open(self, app_id, title=None, **kwargs):
        self.client = self.wm.open_window(app_id, app_id if title is None else title, **kwargs)
        return self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                                   and e["container"]["app_id"] == app_id)["container"]["id"]

    def dimmed(self):
        return {con_id: w["dimmed"] for con_id, w in self.windows().items()}

    def outputs(self, con_id):
        """con_id's output, then the other one."""
        here = self.windows()[con_id]["output"]
        there, = [o["name"] for o in self.connect().request(GET_OUTPUTS) if o["name"] != here]
        return here, there


class DimTest(DimHelpers):
    def test_every_window_but_the_focused_one_is_dimmed(self):
        first = self.open("test.first")
        second = self.open("test.second")
        self.assertEqual(self.dimmed(), {first: True, second: False})
        self.command(self.connect(), f"[con_id={first}] focus")
        self.assertEqual(self.dimmed(), {first: False, second: True})

    def test_a_focus_event_says_the_window_isnt_dimmed(self):
        first = self.open("test.first")
        self.open("test.second")
        self.command(self.connect(), f"[con_id={first}] focus")
        event = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "focus"
                                    and e["container"]["id"] == first)
        self.assertEqual((event["container"]["focused"], event["container"]["dimmed"]),
                         (True, False))

    def test_a_title_event_says_whether_the_window_is_dimmed(self):
        window = self.open("test.app", stdin=True)
        client = self.client
        self.open("test.other")
        client.stdin.write(b"title Picture-in-Picture\n")
        client.stdin.flush()
        event = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "title"
                                    and e["container"]["id"] == window)
        self.assertEqual(event["container"]["dimmed"], False)
        client.stdin.write(b"title Something else\n")
        client.stdin.flush()
        event = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "title"
                                    and e["container"]["id"] == window)
        self.assertEqual(event["container"]["dimmed"], True)

    def test_a_click_goes_through_the_dim_to_the_window(self):
        first = self.open("test.first")
        self.open("test.second")
        self.assertTrue(self.windows()[first]["dimmed"])
        output, = self.connect().request(GET_OUTPUTS)
        rect = self.windows()[first]["rect"]
        subprocess.run([POINTER, "move", str(rect["x"] + 50), str(rect["y"] + 50),
                        str(output["rect"]["width"]), str(output["rect"]["height"]), "click"],
                       env=self.wm.env, check=True, timeout=TIMEOUT)
        seat, = self.connect().request(GET_SEATS)
        self.assertEqual(seat["last_input"]["window"], first)
        self.assertEqual(self.windows()[first]["dimmed"], False)

    def test_picture_in_picture_isnt_dimmed(self):
        window = self.open("test.app")
        pip = self.open("test.pip", title="Picture-in-Picture")
        self.command(self.connect(), f"[con_id={window}] focus")
        self.assertEqual(self.dimmed(), {window: False, pip: False})

    def test_video_isnt_dimmed_and_a_photo_is(self):
        video = self.open("test.video", content_type="video")
        photo = self.open("test.photo", content_type="photo")
        window = self.open("test.app")
        self.assertEqual(self.dimmed(), {video: False, photo: True, window: False})

    def test_the_dim_command_exempts_a_window(self):
        first = self.open("test.first")
        second = self.open("test.second")
        self.command(self.connect(), f"[con_id={first}] dim disable")
        self.assertEqual(self.dimmed(), {first: False, second: False})
        self.assertEqual(self.windows()[first]["dim"], False)
        self.command(self.connect(), f"[con_id={first}] dim toggle")
        self.assertEqual(self.dimmed(), {first: True, second: False})
        self.assertEqual(self.windows()[first]["dim"], True)

    def test_a_strength_out_of_range_is_an_error(self):
        self.wm.write_config("dim_inactive 1.5\n")
        result, = self.connect().request(RUN_COMMAND, "reload")
        self.assertFalse(result["success"], result)
        self.assertIn("dim_inactive", result["error"])
        config = self.connect().request(GET_CONFIG)
        self.assertEqual(config["config"], self.CONFIG)


class DimRuleTest(DimHelpers):
    CONFIG = ('dim_inactive 0.07\n'
              'for_window [app_id="^test.preview$"] dim disable\n')

    def test_a_for_window_rule_exempts_a_window(self):
        preview = self.open("test.preview")
        window = self.open("test.app")
        self.assertEqual(self.dimmed(), {preview: False, window: False})


class NoDimTest(DimHelpers):
    CONFIG = None

    def test_nothing_dims_without_a_strength(self):
        first = self.open("test.first")
        second = self.open("test.second")
        self.assertEqual(self.dimmed(), {first: False, second: False})


class TwoOutputDimTest(DimHelpers):
    OUTPUTS = 2

    def test_a_monitor_showing_one_window_doesnt_dim_it(self):
        lone = self.open("test.lone")
        here, there = self.outputs(lone)
        self.command(self.connect(), f"focus output {there}")
        other = self.open("test.other")
        self.assertEqual(self.dimmed(), {lone: False, other: False})
        # A second window beside it, and there's something to tell apart
        self.command(self.connect(), f"focus output {here}")
        second = self.open("test.second")
        self.assertEqual(self.dimmed(), {lone: True, other: False, second: False})

    def test_a_fullscreen_window_isnt_dimmed(self):
        self.open("test.under")
        full = self.open("test.full")
        here, there = self.outputs(full)
        self.command(self.connect(), f"[con_id={full}] fullscreen enable")
        self.command(self.connect(), f"focus output {there}")
        self.open("test.other")
        self.assertEqual(self.windows()[full]["dimmed"], False)


if __name__ == "__main__":
    unittest.main()
