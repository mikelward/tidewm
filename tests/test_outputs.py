"""tidewm's output settings: the config file's output lines, in sway's
syntax, and the output command (docs/config.md)."""

import os
import subprocess
import unittest

from harness import (EVENT_OUTPUT, EVENT_TICK, GET_CONFIG, GET_OUTPUTS, GET_TREE, ROOT,
                     RUN_COMMAND, SEND_TICK, TIMEOUT, Tidewm, TidewmTest, power, randr)

POINTER = os.path.join(ROOT, "tests", "pointer")


def mode(output):
    current = output["current_mode"]
    return current["width"], current["height"], current["refresh"]


class OutputsTest(TidewmTest):
    def outputs(self):
        return {o["name"]: o for o in self.connect().request(GET_OUTPUTS)}

    def fails(self, command):
        result, = self.connect().request(RUN_COMMAND, command)
        self.assertFalse(result["success"], f"{command!r}: {result}")
        return result


class OneOutputTest(OutputsTest):
    CONFIG = "output HEADLESS-1 mode 800x600@50Hz scale 2\n"

    def test_an_output_line_sets_the_mode_and_scale(self):
        output = self.outputs()["HEADLESS-1"]
        self.assertEqual(mode(output), (800, 600, 50000))
        self.assertEqual(output["scale"], 2)
        # The layout is in logical pixels, so half the mode at scale 2
        self.assertEqual((output["rect"]["width"], output["rect"]["height"]), (400, 300))

    def test_the_output_command_sets_them_now(self):
        self.command(self.connect(), "output HEADLESS-1 scale 1 transform 90")
        output = self.outputs()["HEADLESS-1"]
        self.assertEqual((output["scale"], output["transform"]), (1, "90"))
        # Turned, the mode's width is the layout's height
        self.assertEqual((output["rect"]["width"], output["rect"]["height"]), (600, 800))

    def test_reload_undoes_what_the_output_command_set(self):
        self.command(self.connect(), "output HEADLESS-1 transform 90 scale 3")
        self.command(self.connect(), "reload")
        output = self.outputs()["HEADLESS-1"]
        self.assertEqual((output["scale"], output["transform"]), (2, "normal"))

    def test_a_setting_taken_out_of_the_file_goes_on_reload(self):
        self.wm.write_config("# no output lines\n")
        self.command(self.connect(), "reload")
        self.assertEqual(self.outputs()["HEADLESS-1"]["scale"], 1)

    def test_reload_sets_an_output_lines_new_settings(self):
        self.wm.write_config("output * scale 1.5\n")
        self.command(self.connect(), "reload")
        self.assertEqual(self.outputs()["HEADLESS-1"]["scale"], 1.5)

    def test_reload_fails_naming_an_output_that_refuses_its_settings(self):
        # Too wide for a buffer, it's refused, and the output keeps its mode
        text = "output HEADLESS-1 mode 600000000x1\nbindsym Mod1+x nop\n"
        self.wm.write_config(text)
        result, = self.connect().request(RUN_COMMAND, "reload")
        self.assertFalse(result["success"])
        self.assertIn("output HEADLESS-1 refused its settings, so it keeps what it had; "
                      "the rest of the file is loaded", result["error"])
        self.assertEqual(mode(self.outputs()["HEADLESS-1"]), (800, 600, 50000))
        config = self.connect().request(GET_CONFIG)
        self.assertEqual(config["config"], text)
        self.assertEqual(len(config["errors"]), 1)
        self.assertIn("output HEADLESS-1 refused its settings", config["errors"][0])
        # A reload that works clears it
        self.wm.write_config(self.CONFIG)
        self.command(self.connect(), "reload")
        self.assertEqual(self.connect().request(GET_CONFIG)["errors"], [])

    def test_a_name_and_values_may_be_quoted(self):
        self.wm.write_config('output "HEADLESS-1" mode "640x480@30Hz" scale "1"\n')
        self.command(self.connect(), "reload")
        output = self.outputs()["HEADLESS-1"]
        self.assertEqual((mode(output), output["scale"]), ((640, 480, 30000), 1))
        self.command(self.connect(), 'output "HEADLESS-1" transform "90"')
        self.assertEqual(self.outputs()["HEADLESS-1"]["transform"], "90")

    def test_the_output_command_says_when_no_output_is_called_that(self):
        self.assertIn("No output is called HDMI-A-9", self.fails("output HDMI-A-9 scale 2")["error"])

    def test_output_errors(self):
        for line, error in [
            ("output\n", "output needs a name, or *, then settings"),
            ("output HEADLESS-1\n", "output HEADLESS-1 needs a setting"),
            ("output HEADLESS-1 mode wide\n", "mode wide isn't WIDTHxHEIGHT[@HZ]"),
            ("output HEADLESS-1 mode 800x600junk\n", "mode 800x600junk isn't WIDTHxHEIGHT[@HZ]"),
            ("output HEADLESS-1 mode 800x600@-60\n", "mode 800x600@-60 isn't WIDTHxHEIGHT[@HZ]"),
            ("output HEADLESS-1 adaptive_sync maybe\n", "adaptive_sync takes on or off"),
            ("output HEADLESS-1 dpms dim\n", "dpms takes on or off"),
            ("output HEADLESS-1 scale_filter cubic\n", "scale_filter takes linear, nearest or smart"),
            ('output "HEADLESS-1" {\nscale 2\n', ":1: output HEADLESS-1 has no }"),
            ('output HEADLESS-1 {\nbrightness 50\n}\n', ":2: output HEADLESS-1: no setting is called brightness"),
            ('output HEADLESS-1 mode "800x600\n', 'output HEADLESS-1: a value\'s " has no closing "'),
            ('output "HEADLESS-1 scale 2\n', "output needs a name, or *, then settings"),
            ("output HEADLESS-1 scale 0\n", "scale 0 isn't a number from above 0 to 10"),
            ("output HEADLESS-1 scale nan\n", "scale nan isn't a number from above 0 to 10"),
            ("output HEADLESS-1 position 10\n", "position needs X Y"),
            ("output HEADLESS-1 position auto-sideways\n",
             "position needs X Y, or auto, auto-right, auto-left, auto-up or auto-down"),
            ("output HEADLESS-1 transform 45\n", "no transform is called 45"),
            ("output HEADLESS-1 brightness 50\n", "no setting is called brightness"),
            ("mode m {\noutput * scale 2\n}\n", ":2: output can't be in a mode"),
        ]:
            self.wm.write_config(line)
            result, = self.connect().request(RUN_COMMAND, "reload")
            self.assertFalse(result["success"], line)
            self.assertIn(error, result["error"], line)
        self.assertTrue(self.fails("output HEADLESS-1 scale -1")["parse_error"])
        # Nothing took: the file's settings stand
        self.assertEqual(self.outputs()["HEADLESS-1"]["scale"], 2)
        self.assertEqual(self.connect().request(GET_CONFIG)["config"], self.CONFIG)


class TwoOutputsTest(OutputsTest):
    OUTPUTS = 2
    CONFIG = """output * scale 2
output HEADLESS-1 scale 1 position 2000 0
output HEADLESS-2 pos 0,0
"""

    def test_an_outputs_own_line_overrides_star(self):
        outputs = self.outputs()
        self.assertEqual((outputs["HEADLESS-1"]["scale"], outputs["HEADLESS-2"]["scale"]), (1, 2))

    def test_an_output_line_places_the_output(self):
        outputs = self.outputs()
        self.assertEqual((outputs["HEADLESS-1"]["rect"]["x"], outputs["HEADLESS-2"]["rect"]["x"]),
                         (2000, 0))

    def test_an_output_can_be_disabled_and_enabled_and_its_windows_move(self):
        ipc = self.connect()
        ipc.subscribe("window")
        self.command(self.connect(), "focus output HEADLESS-2")
        _, con = self.open_window(ipc, "test.moving")
        self.assertEqual(con["output"], "HEADLESS-2")
        self.command(self.connect(), "output HEADLESS-2 disable")
        self.assertFalse(self.outputs()["HEADLESS-2"]["active"])
        self.assertEqual(self.windows()[con["id"]]["output"], "HEADLESS-1")
        self.command(self.connect(), "output HEADLESS-2 enable")
        self.assertTrue(self.outputs()["HEADLESS-2"]["active"])

    def test_every_window_of_a_disabled_output_moves(self):
        ipc = self.connect()
        ipc.subscribe("window")
        self.command(self.connect(), "focus output HEADLESS-2")
        cons = [self.open_window(ipc, f"test.moving{i}")[1] for i in range(3)]
        self.command(self.connect(), "output HEADLESS-2 disable")
        windows = self.windows()
        self.assertEqual([windows[c["id"]]["output"] for c in cons], ["HEADLESS-1"] * 3)

    def test_an_output_powered_off_can_still_be_disabled(self):
        ipc = self.connect()
        ipc.subscribe("window")
        self.command(self.connect(), "focus output HEADLESS-2")
        _, con = self.open_window(ipc, "test.asleep")
        power(self.wm, "HEADLESS-2", "off")
        # Asleep, as an idle daemon leaves it, it keeps its windows
        self.assertEqual(self.windows()[con["id"]]["output"], "HEADLESS-2")
        self.command(self.connect(), "output HEADLESS-2 disable")
        self.assertEqual(self.windows()[con["id"]]["output"], "HEADLESS-1")

    def test_the_power_command_turns_an_output_off_and_on_keeping_its_windows(self):
        ipc = self.connect()
        ipc.subscribe("window")
        self.command(self.connect(), "focus output HEADLESS-2")
        _, con = self.open_window(ipc, "test.dozing")
        self.command(self.connect(), "output HEADLESS-2 power off")
        outputs = self.outputs()
        self.assertEqual((outputs["HEADLESS-1"]["power"], outputs["HEADLESS-2"]["power"]), (True, False))
        # Off, as an idle daemon turns it off, it keeps its windows and place
        self.assertEqual(self.windows()[con["id"]]["output"], "HEADLESS-2")
        self.assertEqual(outputs["HEADLESS-2"]["rect"]["x"], 0)
        self.command(self.connect(), "output * power on")
        outputs = self.outputs()
        self.assertTrue(outputs["HEADLESS-2"]["power"])
        self.assertTrue(outputs["HEADLESS-2"]["active"])
        self.assertEqual(outputs["HEADLESS-2"]["scale"], 2)
        self.assertEqual(self.windows()[con["id"]]["output"], "HEADLESS-2")

    def test_the_power_command_turns_every_output_off_and_toggles(self):
        self.command(self.connect(), "output * power off")
        self.assertEqual([o["power"] for o in self.outputs().values()], [False, False])
        # Asked again, it stays off
        self.command(self.connect(), "output * power off")
        self.command(self.connect(), "output HEADLESS-1 power toggle")
        outputs = self.outputs()
        self.assertEqual((outputs["HEADLESS-1"]["power"], outputs["HEADLESS-2"]["power"]), (True, False))
        self.command(self.connect(), "output * power toggle")
        outputs = self.outputs()
        self.assertEqual((outputs["HEADLESS-1"]["power"], outputs["HEADLESS-2"]["power"]), (False, True))

    def test_an_output_powered_off_by_an_idle_daemon_comes_on_by_the_command(self):
        power(self.wm, "HEADLESS-2", "off")
        self.command(self.connect(), "output HEADLESS-2 power on")
        self.assertTrue(self.outputs()["HEADLESS-2"]["power"])

    def test_a_disabled_output_stays_off_whatever_its_power(self):
        self.command(self.connect(), "output HEADLESS-2 disable")
        self.command(self.connect(), "output * power on")
        self.assertFalse(self.outputs()["HEADLESS-2"]["active"])
        result = self.fails("output HEADLESS-2 power on")
        self.assertIn("HEADLESS-2 is disabled", result["error"])

    def test_the_power_command_takes_on_off_or_toggle(self):
        result = self.fails("output HEADLESS-1 power sideways")
        self.assertTrue(result.get("parse_error"), result)
        self.fails("output HEADLESS-1 power off now")
        result = self.fails("output HEADLESS-9 power off")
        self.assertIn("No output is called HEADLESS-9", result["error"])

    def test_an_output_enabled_again_keeps_its_mode(self):
        self.command(self.connect(), "output HEADLESS-1 mode 640x480@30Hz")
        self.command(self.connect(), "output HEADLESS-1 disable")
        self.command(self.connect(), "output HEADLESS-1 enable")
        self.assertEqual(mode(self.outputs()["HEADLESS-1"]), (640, 480, 30000))

    def test_a_mode_set_while_disabled_is_used_once_enabled(self):
        self.command(self.connect(), "output HEADLESS-1 disable")
        self.command(self.connect(), "output HEADLESS-1 mode 640x480@30Hz")
        self.command(self.connect(), "output HEADLESS-1 enable")
        self.assertEqual(mode(self.outputs()["HEADLESS-1"]), (640, 480, 30000))
    def test_an_output_enabled_again_goes_back_where_it_was_put(self):
        self.command(self.connect(), "output HEADLESS-1 disable")
        self.command(self.connect(), "output HEADLESS-1 enable")
        self.assertEqual(self.outputs()["HEADLESS-1"]["rect"]["x"], 2000)

    def test_a_position_set_while_disabled_is_used_once_enabled(self):
        self.command(self.connect(), "output HEADLESS-1 disable")
        self.command(self.connect(), "output HEADLESS-1 position 3000 0")
        self.command(self.connect(), "output HEADLESS-1 enable")
        self.assertEqual(self.outputs()["HEADLESS-1"]["rect"]["x"], 3000)


class FocusedOutputTest(OutputsTest):
    """tidewm's own output event: focus moving to another output, which
    Quickshell's I3 module and tide's shell track (docs/ipc.md). The two
    outputs are side by side, HEADLESS-2 on the left, as it connects
    first."""
    OUTPUTS = 2

    def setUp(self):
        super().setUp()
        self.command(self.connect(), "focus output HEADLESS-1")
        self.listener = self.connect()
        self.listener.subscribe("output", "tick")
        # Subscribing sends a tick at once
        self.listener.next_event(EVENT_TICK)

    def focused(self):
        return [name for name, o in self.outputs().items() if o["focused"]]

    def next_focus(self):
        """The next output focus event's output, or None if a tick sent
        now comes first: nothing more was said. Each call's tick is its
        own, so one an earlier call left queued is passed over."""
        self.ticks = getattr(self, "ticks", 0) + 1
        payload = f"after {self.ticks}"
        self.connect().request(SEND_TICK, payload)
        while True:
            kind, event = self.listener.recv()
            if kind == EVENT_OUTPUT and event["change"] == "focus":
                self.assertEqual(sorted(event), ["change", "output"])
                return event["output"]
            if kind == EVENT_TICK and event["payload"] == payload:
                return None

    def point(self, x, y):
        """Moves the pointer to x, y, as a mouse does, which focus follows."""
        rects = [o["rect"] for o in self.outputs().values()]
        width = max(r["x"] + r["width"] for r in rects)
        height = max(r["y"] + r["height"] for r in rects)
        subprocess.run([POINTER, "cross", str(x), str(y), str(width), str(height)],
                       env=self.wm.env, check=True, timeout=TIMEOUT)

    def middle(self, name):
        rect = self.outputs()[name]["rect"]
        return rect["x"] + rect["width"] // 2, rect["y"] + rect["height"] // 2

    def test_focusing_another_output_says_so(self):
        self.command(self.connect(), "focus output HEADLESS-2")
        self.assertEqual(self.next_focus(), "HEADLESS-2")
        self.assertEqual(self.focused(), ["HEADLESS-2"])

    def test_focusing_the_focused_output_says_nothing(self):
        self.command(self.connect(), "focus output HEADLESS-1")
        self.assertIsNone(self.next_focus())

    def test_the_pointer_crossing_to_another_output_says_so(self):
        self.point(*self.middle("HEADLESS-2"))
        self.assertEqual(self.next_focus(), "HEADLESS-2")
        self.assertEqual(self.focused(), ["HEADLESS-2"])
        self.point(*self.middle("HEADLESS-1"))
        self.assertEqual(self.next_focus(), "HEADLESS-1")

    def test_the_pointer_moving_within_the_output_says_nothing(self):
        x, y = self.middle("HEADLESS-1")
        self.point(x, y)
        self.point(x + 10, y + 10)
        self.assertEqual(self.next_focus(), None)

    def test_focusing_a_window_on_another_output_says_so(self):
        windows = self.connect()
        windows.subscribe("window")
        self.command(self.connect(), "focus output HEADLESS-2")
        _, con = self.open_window(windows, "test.other")
        self.command(self.connect(), "focus output HEADLESS-1")
        self.assertEqual(self.next_focus(), "HEADLESS-2")
        self.assertEqual(self.next_focus(), "HEADLESS-1")
        self.command(self.connect(), f"[con_id={con['id']}] focus")
        self.assertEqual(self.next_focus(), "HEADLESS-2")

    def test_disabling_the_focused_output_says_where_focus_went(self):
        self.command(self.connect(), "focus output HEADLESS-2")
        self.assertEqual(self.next_focus(), "HEADLESS-2")
        self.command(self.connect(), "output HEADLESS-2 disable")
        self.assertEqual(self.next_focus(), "HEADLESS-1")
        self.assertEqual(self.focused(), ["HEADLESS-1"])


class DisabledAtStartTest(OutputsTest):
    OUTPUTS = 2
    CONFIG = "output HEADLESS-2 disable\n"

    def test_an_output_disabled_by_its_line_starts_off(self):
        outputs = self.outputs()
        self.assertEqual((outputs["HEADLESS-1"]["active"], outputs["HEADLESS-2"]["active"]),
                         (True, False))


class RefusedAtStartTest(OutputsTest):
    CONFIG = "output HEADLESS-1 mode 600000000x1\n"

    def test_an_output_refusing_its_line_at_startup_gets_its_own_mode_and_says_so(self):
        output = self.outputs()["HEADLESS-1"]
        self.assertTrue(output["active"])
        self.assertEqual(mode(output), (1280, 720, 0))
        errors = self.connect().request(GET_CONFIG)["errors"]
        self.assertEqual(len(errors), 1)
        self.assertIn("output HEADLESS-1 refused its settings, so it has its preferred mode",
                      errors[0])


class FirstDisabledAtStartTest(OutputsTest):
    OUTPUTS = 2
    CONFIG = "output HEADLESS-1 disable\n"

    def enabled(self):
        """Each output and whether it's on, as output management says."""
        heads = {}
        for line in randr(self).splitlines():
            if not line.startswith(" "):
                name = line.split()[0]
            elif line.strip().startswith("Enabled:"):
                heads[name] = line.split()[-1]
        return heads

    def test_output_management_can_see_and_enable_an_output_disabled_by_its_line(self):
        self.assertEqual(self.enabled(), {"HEADLESS-1": "no", "HEADLESS-2": "yes"})
        randr(self, "--output", "HEADLESS-1", "--on")
        self.assertTrue(self.outputs()["HEADLESS-1"]["active"])


class DisabledWithAModeTest(OutputsTest):
    OUTPUTS = 2
    CONFIG = "output HEADLESS-2 mode 640x480@30Hz disable\n"

    def test_an_output_disabled_with_a_mode_starts_off_then_gets_it(self):
        self.assertFalse(self.outputs()["HEADLESS-2"]["active"])
        self.command(self.connect(), "output HEADLESS-2 enable")
        self.assertEqual(mode(self.outputs()["HEADLESS-2"]), (640, 480, 30000))


class OutputIdentifierTest(OutputsTest):
    """An output named by what it is, as sway's identifier names it, "MAKE
    MODEL SERIAL" with Unknown for what it doesn't say: the headless
    backend's outputs say nothing."""
    CONFIG = 'output "Unknown Unknown Unknown" scale 2\noutput HEADLESS-1 scale 3\n'

    def test_a_line_naming_what_it_is_overrides_one_naming_its_connector(self):
        output = self.outputs()["HEADLESS-1"]
        self.assertEqual((output["make"], output["model"], output["serial"]), ("", "", ""))
        self.assertEqual(output["scale"], 2)

    def test_the_output_command_can_name_what_it_is(self):
        self.command(self.connect(), 'output "Unknown Unknown Unknown" scale 1.5')
        self.assertEqual(self.outputs()["HEADLESS-1"]["scale"], 1.5)


class DescribedOutputTest(OutputsTest):
    """An output named by Hyprland's description, which leaves out what it
    doesn't say, as tide's Displays page names it: tidewm nested on X,
    where an output's make is the X server's vendor and its model the
    protocol's version, with no serial. The X server is the parent's
    Xwayland."""

    def setUp(self):
        self.parent = parent = Tidewm()
        self.addCleanup(parent.stop)
        self.need(parent.x11, "an output with a make needs Xwayland")
        self.wm = Tidewm(1, "output * scale 1\n", parent=parent, on_x11=True)
        self.addCleanup(self.wm.stop)
        self.output, = self.connect().request(GET_OUTPUTS)

    def scale(self):
        return self.outputs()[self.output["name"]]["scale"]

    def test_its_description_and_identifier_name_it(self):
        make, model, serial = self.output["make"], self.output["model"], self.output["serial"]
        self.assertTrue(make and model, self.output)
        self.assertEqual(serial, "")
        self.command(self.connect(), f'output "{make} {model}" scale 2')
        self.assertEqual(self.scale(), 2)
        self.command(self.connect(), f'output "{make} {model} Unknown" scale 1.5')
        self.assertEqual(self.scale(), 1.5)

    def test_with_the_missing_serials_space_it_names_nothing(self):
        make, model = self.output["make"], self.output["model"]
        self.assertIn("No output is called", self.fails(f'output "{make} {model} " scale 2')["error"])

    def test_a_line_naming_its_description_overrides_one_naming_its_connector(self):
        make, model = self.output["make"], self.output["model"]
        self.wm.write_config(f'output "{make} {model}" scale 2\noutput {self.output["name"]} scale 3\n')
        self.command(self.connect(), "reload")
        self.assertEqual(self.scale(), 2)


class PlacedBesideTest(OutputsTest):
    """position auto, auto-right, auto-left, auto-up and auto-down, which
    place an output beside the others as Hyprland's monitor rules do: in
    the order they were connected, beside those put at positions and those
    placed before it, and the origin, at 0 across. Without a position, an
    output goes to the right. The headless backend connects HEADLESS-2
    first, so a test that needs one where it is puts HEADLESS-1 at 0, 0."""
    OUTPUTS = 2
    CONFIG = "output * scale 1\n"
    AT_ORIGIN = "output HEADLESS-1 position 0 0\n"

    def rects(self):
        return {name: (o["rect"]["x"], o["rect"]["y"], o["rect"]["width"], o["rect"]["height"])
                for name, o in self.outputs().items()}

    def reload(self, config):
        self.wm.write_config(config)
        self.command(self.connect(), "reload")

    def test_outputs_without_positions_sit_side_by_side(self):
        rects = self.rects()
        width = rects["HEADLESS-1"][2]
        self.assertEqual(sorted(r[:2] for r in rects.values()), [(0, 0), (width, 0)])

    def test_each_side(self):
        _, _, width, height = self.rects()["HEADLESS-1"]
        for position, where in [
            ("auto", (width, 0)),
            ("auto-right", (width, 0)),
            ("auto-left", (-width, 0)),
            ("auto-up", (0, -height)),
            ("auto-down", (0, height)),
        ]:
            with self.subTest(position):
                self.reload(f"{self.AT_ORIGIN}output HEADLESS-2 position {position}\n")
                rects = self.rects()
                self.assertEqual(rects["HEADLESS-1"][:2], (0, 0))
                self.assertEqual(rects["HEADLESS-2"][:2], where)

    def test_the_output_command_places_it_now_until_reload(self):
        _, _, width, height = self.rects()["HEADLESS-1"]
        self.reload(self.AT_ORIGIN)
        self.command(self.connect(), "output HEADLESS-2 position auto-up")
        self.assertEqual(self.rects()["HEADLESS-2"][:2], (0, -height))
        self.command(self.connect(), "reload")
        self.assertEqual(self.rects()["HEADLESS-2"][:2], (width, 0))

    def test_it_goes_beside_those_put_at_positions(self):
        self.reload("output HEADLESS-1 position 2000 100\noutput HEADLESS-2 position auto-down\n")
        rects = self.rects()
        self.assertEqual(rects["HEADLESS-1"][:2], (2000, 100))
        self.assertEqual(rects["HEADLESS-2"][:2], (0, 100 + rects["HEADLESS-1"][3]))

    def test_it_moves_when_the_one_beside_it_changes_size_or_goes(self):
        _, _, width, height = self.rects()["HEADLESS-1"]
        self.reload(f"{self.AT_ORIGIN}output HEADLESS-2 position auto-down\n")
        self.command(self.connect(), "output HEADLESS-1 scale 2")
        self.assertEqual(self.rects()["HEADLESS-2"][:2], (0, height // 2))
        self.command(self.connect(), "output HEADLESS-1 disable")
        self.assertEqual(self.rects()["HEADLESS-2"][:2], (0, 0))
        self.command(self.connect(), "output HEADLESS-1 enable")
        rects = self.rects()
        self.assertEqual(rects["HEADLESS-1"][:2], (0, 0))
        self.assertEqual(rects["HEADLESS-2"][:2], (0, height // 2))

    def test_one_output_management_moves_stays_where_it_was_put(self):
        randr(self, "--output", "HEADLESS-2", "--pos", "5000,0")
        self.assertEqual(self.rects()["HEADLESS-2"][:2], (5000, 0))
        self.command(self.connect(), "output HEADLESS-1 scale 2")
        self.assertEqual(self.rects()["HEADLESS-2"][:2], (5000, 0))

    def test_the_one_output_management_leaves_stays_beside_the_one_it_moves(self):
        # Its head, as the client last saw it, isn't it being moved, even
        # once moving the other has placed it beside that one. Each order,
        # so whichever head comes first, the other is the one after it.
        for moved, other in [("HEADLESS-1", "HEADLESS-2"), ("HEADLESS-2", "HEADLESS-1")]:
            with self.subTest(moved):
                self.command(self.connect(), "reload")
                randr(self, "--output", moved, "--pos", "5000,0")
                width = self.rects()[moved][2]
                self.assertEqual(self.rects()[other][:2], (5000 + width, 0))
                self.command(self.connect(), f"output {moved} scale 2")
                self.assertEqual(self.rects()[other][:2], (5000 + width // 2, 0))


class PlacedBesideAtStartTest(OutputsTest):
    """Each output is placed beside the others as it connects."""
    OUTPUTS = 2
    CONFIG = "output HEADLESS-1 position auto-left\n"

    def test_an_output_is_placed_as_it_connects(self):
        outputs = self.outputs()
        first, second = outputs["HEADLESS-1"]["rect"], outputs["HEADLESS-2"]["rect"]
        self.assertEqual((first["x"], first["y"]), (-first["width"], 0))
        self.assertEqual((second["x"], second["y"]), (0, 0))


# The file nwg-displays saves for sway, ~/.config/sway/outputs, as its
# settings_applier writes it: a header, then a block for each output it
# shows, mode with an empty --custom, then a line for each it turned off.
NWG_DISPLAYS_FILE = """# Generated by nwg-displays on 2026-11-08 at 12:00:00. Do not edit manually.

output "HEADLESS-1" {
    mode  1280x720@0.0Hz
    pos 0 0
    transform normal
    scale 2.0
    scale_filter linear
    adaptive_sync off
    dpms on
}
output "HEADLESS-2" {
    mode --custom 1024x768@60.0Hz
    pos 640 0
    transform 90
    scale 1.0
    scale_filter nearest
    adaptive_sync on
    dpms on
}
"""


def nwg_displays_outputs(tree):
    """What nwg-displays' list_outputs reads from GET_TREE under sway,
    each field as it reads it, with no fallback where it has none."""
    found = {}
    for item in tree["nodes"]:
        if item["type"] != "output" or item["name"].startswith("__"):
            continue
        found[item["name"]] = {
            "x": item["rect"]["x"], "y": item["rect"]["y"],
            "logical-width": item["rect"]["width"], "logical-height": item["rect"]["height"],
            "physical-width": item["current_mode"]["width"],
            "physical-height": item["current_mode"]["height"],
            "active": item["active"], "dpms": item["dpms"], "transform": item["transform"],
            "scale": float(item["scale"]), "scale_filter": item["scale_filter"],
            "adaptive_sync_status": item["adaptive_sync_status"],
            "refresh": item["current_mode"]["refresh"] / 1000, "modes": item["modes"],
            "description": "{} {} {}".format(item["make"], item["model"], item["serial"]),
            "focused": item["focused"],
        }
    return found


class NwgDisplaysTest(OutputsTest):
    """nwg-displays, which takes tidewm for sway by its SWAYSOCK: what it
    reads over the IPC, what it sends, and the file it saves, which tide's
    config includes."""
    OUTPUTS = 2
    CONFIG = "include outputs\n"

    def save(self, text):
        path = os.path.join(os.path.dirname(self.wm.config_path), "outputs")
        with open(path, "w") as f:
            f.write(text)

    def tree_outputs(self):
        return nwg_displays_outputs(self.connect().request(GET_TREE))

    def test_get_outputs_and_the_tree_give_what_nwg_displays_reads_as_sway_types_it(self):
        listed = self.outputs()
        self.assertEqual(set(self.tree_outputs()), {"HEADLESS-1", "HEADLESS-2"})
        for name, output in self.tree_outputs().items():
            with self.subTest(name):
                for field in ("active", "dpms", "focused"):
                    self.assertIsInstance(output[field], bool, field)
                self.assertEqual((output["active"], output["dpms"]), (True, True))
                self.assertEqual(output["scale_filter"], "linear")
                self.assertEqual(output["adaptive_sync_status"], "disabled")
                self.assertIsInstance(output["modes"], list)
                self.assertIsInstance(output["transform"], str)
                self.assertIsInstance(output["physical-width"], int)
                self.assertEqual(output["refresh"], listed[name]["current_mode"]["refresh"] / 1000)
                for field in ("dpms", "power", "scale_filter", "adaptive_sync_status", "modes"):
                    self.assertIn(field, listed[name])
                self.assertEqual(listed[name]["dpms"], listed[name]["power"])

    def test_the_file_nwg_displays_saves_loads_and_applies(self):
        self.save(NWG_DISPLAYS_FILE)
        self.command(self.connect(), "reload")
        self.assertEqual(self.connect().request(GET_CONFIG)["errors"], [])
        outputs = self.tree_outputs()
        first, second = outputs["HEADLESS-1"], outputs["HEADLESS-2"]
        self.assertEqual((first["scale"], first["x"], first["logical-width"]), (2, 0, 640))
        self.assertEqual((first["physical-width"], first["physical-height"]), (1280, 720))
        self.assertEqual((second["physical-width"], second["physical-height"], second["refresh"]),
                         (1024, 768, 60))
        self.assertEqual((second["transform"], second["x"], second["logical-width"]),
                         ("90", 640, 768))
        # The headless backend has no adaptive sync, so it stays off, as sway leaves it
        self.assertEqual(second["adaptive_sync_status"], "disabled")

    def test_an_output_nwg_displays_turned_off_starts_off_and_leaves_the_tree(self):
        first_only = NWG_DISPLAYS_FILE.split('output "HEADLESS-2"')[0]
        self.save(first_only + 'output "HEADLESS-2" disable\n')
        self.command(self.connect(), "reload")
        self.assertFalse(self.outputs()["HEADLESS-2"]["active"])
        self.assertEqual(set(self.tree_outputs()), {"HEADLESS-1"})

    def test_the_commands_nwg_displays_sends_apply(self):
        # As its settings_applier sends them on Apply, and its toggles
        self.command(self.connect(), 'output "HEADLESS-2" mode  1024x768@60.0Hz pos 1280 0 '
                     "transform 180 scale 2.0 scale_filter linear adaptive_sync off dpms off")
        outputs, listed = self.tree_outputs(), self.outputs()
        second = outputs["HEADLESS-2"]
        self.assertEqual((second["transform"], second["scale"], second["x"]), ("180", 2, 1280))
        # Powered off, it's still active, and in the tree, as in sway
        self.assertEqual((second["active"], second["dpms"], listed["HEADLESS-2"]["power"]),
                         (True, False, False))
        self.command(self.connect(), 'output "HEADLESS-2" mode --custom 800x600@30.0Hz dpms on')
        second = self.tree_outputs()["HEADLESS-2"]
        self.assertEqual((second["dpms"], second["physical-width"], second["refresh"]),
                         (True, 800, 30))
        self.command(self.connect(), "output HEADLESS-2 disable")
        self.assertEqual(set(self.tree_outputs()), {"HEADLESS-1"})
        self.command(self.connect(), "output HEADLESS-2 enable")
        self.assertTrue(self.tree_outputs()["HEADLESS-2"]["active"])

    def test_adaptive_sync_on_is_taken_where_an_output_cant_have_it(self):
        self.command(self.connect(), "output HEADLESS-1 adaptive_sync on")
        self.assertEqual(self.outputs()["HEADLESS-1"]["adaptive_sync_status"], "disabled")

    def test_dpms_alone_is_power(self):
        self.command(self.connect(), "output HEADLESS-1 dpms off")
        self.assertFalse(self.outputs()["HEADLESS-1"]["power"])
        self.command(self.connect(), "output HEADLESS-1 dpms toggle")
        self.assertTrue(self.outputs()["HEADLESS-1"]["power"])


class OutputPowerInTheFileTest(OutputsTest):
    """power or dpms in an output line is taken but left: every output
    starts on, as the file nwg-displays saves says dpms off for one that
    was off when it saved."""
    CONFIG = "output HEADLESS-1 dpms off scale 2\n"

    def test_an_output_line_with_dpms_off_still_starts_on(self):
        output = self.outputs()["HEADLESS-1"]
        self.assertEqual((output["power"], output["scale"]), (True, 2))


if __name__ == "__main__":
    unittest.main()
