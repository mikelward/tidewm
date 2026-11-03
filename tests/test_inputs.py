"""tidewm's input settings: the config file's input lines and blocks, in
sway's syntax, the input command, GET_INPUTS and the input event
(docs/config.md).

The headless backend has no input devices, so the keyboard tests run
tidewm nested in another, whose Wayland backend gives it a keyboard of
its own. Nothing here has libinput: its settings are only parsed."""

import os
import select
import shutil
import subprocess
import unittest

from harness import (EVENT_INPUT, GET_CONFIG, GET_INPUTS, ROOT, RUN_COMMAND, TIMEOUT, Tidewm,
                     TidewmTest)


class InputTest(TidewmTest):
    def fails(self, command):
        result, = self.connect().request(RUN_COMMAND, command)
        self.assertFalse(result["success"], f"{command!r}: {result}")
        return result


class InputErrorsTest(InputTest):
    CONFIG = "input type:touchpad tap enabled\n"

    def test_input_errors(self):
        for text, error in [
            ("input\n", ":1: input needs a device's identifier, a type:, or *, then a setting"),
            ("input type:keyboard\n", ":1: input type:keyboard needs a setting"),
            ("input type:mouse tap enabled\n", ":1: input type:mouse: no device type is called mouse"),
            ("input type:mouse {\n}\n", ":1: input type:mouse: no device type is called mouse"),
            ('input "type:keyboard repeat_rate 30\n',
             ":1: input needs a device's identifier, a type:, or *, then a setting"),
            ('input * tap "enabled\n', ':1: input *: tap\'s " has no closing "'),
            ('input * tap ""\n', ":1: input *: tap can't be "),
            ("input * tap maybe\n", ":1: input *: tap can't be maybe"),
            ("input * tap\n", ":1: input *: tap needs one value"),
            ("input * tap enabled now\n", ":1: input *: tap needs one value"),
            ("input * brightness 5\n", ":1: input *: no setting is called brightness"),
            ("input * pointer_accel 2\n", ":1: input *: pointer_accel 2 isn't a number from -1 to 1"),
            ("input * pointer_accel nan\n", ":1: input *: pointer_accel nan isn't a number from -1 to 1"),
            ("input * repeat_rate fast\n",
             ":1: input *: repeat_rate fast isn't a whole number from 0 to 100000"),
            ("input type:keyboard xkb_layout nosuchlayout\n",
             ":1: input type:keyboard: XKB can't make a keymap of its xkb_ settings"),
            ("input * {\ntap enabled\n", ":1: input * has no }"),
            ("input * {\nbrightness 5\n}\n", ":2: input *: no setting is called brightness"),
            ("mode m {\ninput * tap enabled\n}\n", ":2: input can't be in a mode"),
            ("}\n", ":1: } closes no mode or input block"),
        ]:
            self.wm.write_config(text)
            result, = self.connect().request(RUN_COMMAND, "reload")
            self.assertFalse(result["success"], text)
            self.assertIn(error, result["error"], text)
        # Nothing took: the file's settings stand
        self.assertEqual(self.connect().request(GET_CONFIG)["config"], self.CONFIG)

    def test_lines_and_blocks_load(self):
        self.wm.write_config("""set $kb type:keyboard
input $kb {
    xkb_layout us,de
    xkb_options grp:alt_shift_toggle
    repeat_rate 30
}
input type:touchpad {
    tap enabled
    natural_scroll enabled
    scroll_method two_finger
    pointer_accel -0.5
}
input * events enabled
""")
        self.command(self.connect(), "reload")

    def test_without_devices_get_inputs_is_empty(self):
        self.assertEqual(self.connect().request(GET_INPUTS), [])

    def test_the_input_command_fails_when_no_device_matches(self):
        self.assertIn("No input device matches type:touchpad",
                      self.fails("input type:touchpad tap enabled")["error"])

    def test_the_input_command_parses_as_a_line_does(self):
        for command in ("input", "input type:mouse tap enabled", "input * tap",
                        "input * tap maybe", "input * brightness 5"):
            self.assertTrue(self.fails(command)["parse_error"], command)


class NestedTest(InputTest):
    """tidewm nested in another, with a keyboard of its own. wtype's
    virtual keyboard gives the parent's seat a keyboard, which it keeps,
    as dwl does, after wtype is done, so the nested tidewm gets one."""

    def setUp(self):
        self.need(shutil.which("wtype"), "a nested keyboard needs wtype")
        self.parent = parent = Tidewm()
        self.addCleanup(parent.stop)
        parent.press("-k", "Shift_L")
        self.wm = Tidewm(self.OUTPUTS, self.CONFIG, self.CONFIG_ARG, parent=parent)
        self.addCleanup(self.wm.stop)

    def keyboard(self):
        """The nested tidewm's keyboard, as GET_INPUTS has it, once it's
        there."""
        ipc = self.connect()
        ipc.subscribe("input")
        for device in self.connect().request(GET_INPUTS):
            if device["type"] == "keyboard":
                return device
        return ipc.next_event(EVENT_INPUT, lambda e: e["change"] == "added"
                              and e["input"]["type"] == "keyboard")["input"]


class KeyboardTest(NestedTest):
    CONFIG = """input type:keyboard {
    xkb_layout us,de
    repeat_rate 40
}
input type:pointer repeat_delay 100
"""

    def test_a_keyboard_takes_its_types_lines(self):
        keyboard = self.keyboard()
        self.assertRegex(keyboard["identifier"], r"^0:0:\S+$")
        self.assertEqual((keyboard["vendor"], keyboard["product"]), (0, 0))
        self.assertEqual(keyboard["xkb_layout_names"], ["English (US)", "German"])
        self.assertEqual((keyboard["xkb_active_layout_index"], keyboard["xkb_active_layout_name"]),
                         (0, "English (US)"))
        # The pointer's line isn't the keyboard's: its delay is config.h's
        self.assertEqual((keyboard["repeat_rate"], keyboard["repeat_delay"]), (40, 600))
        self.assertNotIn("libinput", keyboard)

    def test_a_keyboards_own_line_overrides_its_types(self):
        identifier = self.keyboard()["identifier"]
        self.wm.write_config(self.CONFIG + f"input {identifier} xkb_layout fr\n")
        self.command(self.connect(), "reload")
        keyboard = self.keyboard()
        self.assertEqual((keyboard["xkb_layout_names"], keyboard["repeat_rate"]), (["French"], 40))

    def test_names_and_values_may_be_quoted(self):
        identifier = self.keyboard()["identifier"]
        self.wm.write_config(f'input "type:keyboard" repeat_rate "30"\ninput "{identifier}" {{\n'
                             '    xkb_layout "fr"\n    xkb_variant ""\n}\n')
        self.command(self.connect(), "reload")
        keyboard = self.keyboard()
        self.assertEqual((keyboard["xkb_layout_names"], keyboard["repeat_rate"]), (["French"], 30))
        self.command(self.connect(), f'input "{identifier}" xkb_layout "de"')
        self.assertEqual(self.keyboard()["xkb_layout_names"], ["German"])

    def test_the_input_command_sets_a_keyboard_until_a_reload(self):
        self.keyboard()
        ipc = self.connect()
        ipc.subscribe("input")
        self.command(self.connect(), "input type:keyboard xkb_layout fr")
        event = ipc.next_event(EVENT_INPUT, lambda e: e["change"] == "xkb_keymap")
        self.assertEqual(event["input"]["xkb_layout_names"], ["French"])
        # What it didn't set, it keeps
        self.assertEqual(self.keyboard()["repeat_rate"], 40)
        self.command(self.connect(), "input * repeat_delay 250")
        self.assertEqual((self.keyboard()["xkb_layout_names"], self.keyboard()["repeat_delay"]),
                         (["French"], 250))
        self.command(self.connect(), "reload")
        keyboard = self.keyboard()
        self.assertEqual((keyboard["xkb_layout_names"], keyboard["repeat_delay"]),
                         (["English (US)", "German"], 600))

    def test_the_input_command_fails_for_a_layout_xkb_lacks(self):
        self.keyboard()
        self.assertIn("XKB can't make a keymap of those settings",
                      self.fails("input type:keyboard xkb_layout nosuchlayout")["error"])
        self.assertEqual(self.keyboard()["xkb_layout_names"], ["English (US)", "German"])

    def test_lines_that_make_no_keymap_together_are_an_error(self):
        # Each is a keymap alone, over config.h's, but de has no colemak
        self.keyboard()
        self.wm.write_config("input type:keyboard xkb_layout de\ninput * xkb_variant colemak\n")
        result, = self.connect().request(RUN_COMMAND, "reload")
        self.assertFalse(result["success"])
        self.assertIn(":1: input type:keyboard: XKB can't make a keymap of its xkb_ settings",
                      result["error"])
        self.assertEqual(self.connect().request(GET_CONFIG)["config"], self.CONFIG)
        self.assertEqual(self.keyboard()["xkb_layout_names"], ["English (US)", "German"])

    def test_a_variant_takes_its_layout_from_a_broader_line(self):
        # us has no neo, but de, from *'s line, does
        self.wm.write_config("input * xkb_layout de\ninput type:keyboard xkb_variant neo\n")
        self.command(self.connect(), "reload")
        self.assertEqual(self.keyboard()["xkb_layout_names"], ["German (Neo 2)"])


class KeyBindingTest(NestedTest):
    """wtype's keymap puts the first key it types at keycode 1, which the
    nested tidewm's keymaps make Escape, so typing x in the parent presses
    Escape in it."""

    CONFIG = """input type:keyboard xkb_layout de
bindsym Escape input type:keyboard xkb_layout fr
"""

    def test_a_binding_can_move_its_own_keyboard_to_another_group(self):
        # The binding empties the group whose key is still being handled
        self.assertEqual(self.keyboard()["xkb_layout_names"], ["German"])
        ipc = self.connect()
        ipc.subscribe("input")
        self.parent.press("x")
        event = ipc.next_event(EVENT_INPUT, lambda e: e["change"] == "xkb_keymap")
        self.assertEqual(event["input"]["xkb_layout_names"], ["French"])
        # tidewm lived through it, and the group's next key works
        self.parent.press("x")
        self.assertEqual(self.keyboard()["xkb_layout_names"], ["French"])


class InputMethodTest(NestedTest):
    """tests/ime grabs the keyboard over input-method-v2, as an input
    method does to compose text."""

    CONFIG = "input type:keyboard repeat_rate 40\n"

    def line(self, ime):
        self.assertTrue(select.select([ime.stdout], [], [], TIMEOUT)[0], "tests/ime said nothing")
        return ime.stdout.readline()

    def test_an_input_method_gets_the_keys_of_a_keyboard_with_its_own_group(self):
        # Its repeat_rate puts the keyboard in a group of its own, not kb_group
        self.keyboard()
        ime = subprocess.Popen([os.path.join(ROOT, "tests", "ime")], env=self.wm.env,
                               stdin=subprocess.DEVNULL, stdout=subprocess.PIPE)
        self.wm.clients.append(ime)
        self.addCleanup(ime.stdout.close)
        self.assertEqual(self.line(ime), b"grabbed\n")
        self.parent.press("x")  # Escape, keycode 1, in the nested tidewm
        self.assertEqual(self.line(ime), b"key 1 1\n")


class NoConfigKeyboardTest(NestedTest):
    def test_without_a_file_a_keyboard_has_config_hs_settings(self):
        keyboard = self.keyboard()
        self.assertEqual((keyboard["repeat_rate"], keyboard["repeat_delay"]), (25, 600))
        self.assertEqual(len(keyboard["xkb_layout_names"]), 1)


if __name__ == "__main__":
    unittest.main()
