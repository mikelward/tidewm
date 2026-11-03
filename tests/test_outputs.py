"""tidewm's output settings: the config file's output lines, in sway's
syntax, and the output command (docs/config.md)."""

import unittest

from harness import GET_CONFIG, GET_OUTPUTS, RUN_COMMAND, TidewmTest, power, randr


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
            ("output HEADLESS-1 mode 800x600@0\n", "mode 800x600@0 isn't WIDTHxHEIGHT[@HZ]"),
            ('output HEADLESS-1 mode "800x600\n', 'output HEADLESS-1: a value\'s " has no closing "'),
            ('output "HEADLESS-1 scale 2\n', "output needs a name, or *, then settings"),
            ("output HEADLESS-1 scale 0\n", "scale 0 isn't a number from above 0 to 10"),
            ("output HEADLESS-1 scale nan\n", "scale nan isn't a number from above 0 to 10"),
            ("output HEADLESS-1 position 10\n", "position needs X Y"),
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


if __name__ == "__main__":
    unittest.main()
