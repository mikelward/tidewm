"""The mouse from the config file (docs/config.md): sway's
floating_modifier, so Super and a drag move or resize a floating window,
as tide's SPEC §6.6 has it, and sway's --whole-window button bindings, as
tide's Super+middle-click maximizing. config.h's Alt drags stay until the
file says what the mouse does."""

import contextlib
import os
import shutil
import subprocess
import unittest

from harness import (EVENT_BINDING, EVENT_WINDOW, GET_BINDINGS, GET_OUTPUTS,
                     ROOT, RUN_COMMAND, TIMEOUT, TidewmTest)

POINTER = os.path.join(ROOT, "tests", "pointer")

# Holding a modifier: its binding with x runs, which says the modifier is
# down, as often as it's held
HOLD = ('bindsym Mod4+x nop super\n'
        'bindsym Mod1+x nop alt\n'
        'bindsym Mod4+Shift+x nop super+shift\n'
        'for_window [app_id="^test.float$"] floating enable\n')


def is_mouse(event):
    """Whether a binding event is a button's or a scroll's."""
    return event["binding"]["input_type"] == "mouse"


class MouseTest(TidewmTest):
    """Helpers, with no tests of their own."""

    def setUp(self):
        super().setUp()
        self.ipc = self.connect()
        self.ipc.subscribe("window")

    def floating(self):
        """Opens a window that floats, in the middle, and returns its id."""
        _, con = self.open_window(self.ipc, "test.float")
        return con["id"]

    def rect(self, con_id):
        return self.windows()[con_id]["rect"]

    @contextlib.contextmanager
    def holding(self, mod, binding):
        """Holds mod down, wtype's name for it, or several joined by +, as
        wtype does while it waits on its input: its binding's nop, "nop
        binding", running says when."""
        self.need(shutil.which("wtype"), "holding a key needs wtype")
        bindings = self.connect()
        bindings.subscribe("binding")
        mods = [a for m in mod.split("+") for a in ("-M", m)]
        typist = subprocess.Popen(["wtype", *mods, "-k", "x", "-"], env=self.wm.env,
                                  stdin=subprocess.PIPE)
        self.wm.clients.append(typist)
        bindings.next_event(EVENT_BINDING, lambda e: e["binding"]["command"]
                            == f"nop {binding}")
        yield
        typist.stdin.close()
        typist.wait(TIMEOUT)

    def pointer(self, *actions):
        """Does the pointer's actions, which tidewm has all had once it
        returns."""
        subprocess.run([POINTER, *actions], env=self.wm.env, check=True, timeout=TIMEOUT)

    def layout_size(self):
        outputs = self.connect().request(GET_OUTPUTS)
        return [str(max(o["rect"]["x"] + o["rect"]["width"] for o in outputs)),
                str(max(o["rect"]["y"] + o["rect"]["height"] for o in outputs))]

    def over(self, con_id):
        """The pointer's move to 20 px into con_id."""
        rect = self.rect(con_id)
        return ["move", str(rect["x"] + 20), str(rect["y"] + 20), *self.layout_size()]

    def drag(self, con_id, button, x, y):
        """Drags con_id with button from 20 px into it to x, y."""
        self.pointer(*self.over(con_id), "press", button,
                     "move", str(x), str(y), *self.layout_size(), "release", button)

    def moved_by(self, con_id, mod, binding, button):
        """Drags con_id 100 px right and 50 down with button and mod held,
        and returns how far it moved."""
        before = self.rect(con_id)
        with self.holding(mod, binding):
            self.drag(con_id, button, before["x"] + 120, before["y"] + 70)
        after = self.rect(con_id)
        return after["x"] - before["x"], after["y"] - before["y"]

    def resized_to(self, con_id, mod, binding, button):
        """Drags con_id's corner with button and mod held to make it 300 by
        200, as a resize does, and returns its size."""
        before = self.rect(con_id)
        with self.holding(mod, binding):
            self.drag(con_id, button, before["x"] + 300, before["y"] + 200)
        after = self.rect(con_id)
        return after["width"], after["height"]

    def fails_to_load(self, text):
        self.wm.write_config(text)
        result, = self.connect().request(RUN_COMMAND, "reload")
        self.assertFalse(result["success"], result)
        return result["error"]


class FloatingModifierTest(MouseTest):
    CONFIG = "floating_modifier Mod4 normal\n" + HOLD

    def test_super_and_the_left_button_move_a_floating_window(self):
        window = self.floating()
        self.assertEqual(self.moved_by(window, "logo", "super", "left"), (100, 50))

    def test_super_and_the_right_button_resize_a_floating_window(self):
        window = self.floating()
        self.assertEqual(self.resized_to(window, "logo", "super", "right"), (300, 200))

    def test_config_h_alt_drags_are_left_out(self):
        window = self.floating()
        self.assertEqual(self.moved_by(window, "alt", "alt", "left"), (0, 0))

    def test_without_super_a_drag_goes_to_the_window(self):
        window = self.floating()
        before = self.rect(window)
        self.drag(window, "left", before["x"] + 120, before["y"] + 70)
        self.assertEqual(self.rect(window), before)


class TwoModifiersTest(MouseTest):
    CONFIG = "floating_modifier Mod4+Shift\n" + HOLD

    def test_every_modifier_must_be_held(self):
        window = self.floating()
        self.assertEqual(self.moved_by(window, "logo", "super", "left"), (0, 0))
        self.assertEqual(self.moved_by(window, "logo+shift", "super+shift", "left"), (100, 50))


class InverseTest(MouseTest):
    CONFIG = "floating_modifier Mod4 inverse\n" + HOLD

    def test_inverse_swaps_the_buttons(self):
        window = self.floating()
        self.assertEqual(self.resized_to(window, "logo", "super", "left"), (300, 200))
        self.assertEqual(self.moved_by(window, "logo", "super", "right"), (100, 50))


class NoFloatingModifierTest(MouseTest):
    CONFIG = "floating_modifier none\n" + HOLD

    def test_none_drags_with_no_modifier(self):
        window = self.floating()
        self.assertEqual(self.moved_by(window, "logo", "super", "left"), (0, 0))
        self.assertEqual(self.moved_by(window, "alt", "alt", "left"), (0, 0))


class ConfigHButtonsTest(MouseTest):
    """A file that says nothing of the mouse keeps config.h's buttons."""
    CONFIG = HOLD

    def test_alt_still_drags(self):
        window = self.floating()
        self.assertEqual(self.moved_by(window, "alt", "alt", "left"), (100, 50))


class ReboundButtonTest(MouseTest):
    """A file whose only button binding a rebind takes away still said what
    the mouse does, so config.h's buttons stay out."""
    CONFIG = ("bindsym --name float --whole-window Mod1+button2 floating toggle\n"
              "rebind float none\n" + HOLD)

    def test_alt_doesnt_drag(self):
        window = self.floating()
        self.assertEqual(self.moved_by(window, "alt", "alt", "left"), (0, 0))


class ButtonBindingTest(MouseTest):
    CONFIG = ("floating_modifier Mod4\n"
              "bindsym --whole-window Mod4+button2 maximize toggle\n"
              "bindsym --whole-window Mod4+button4 nop scrolled up\n"
              "bindsym --whole-window Mod4+button5 nop scrolled down\n" + HOLD)

    def test_super_and_the_middle_button_run_its_binding_on_the_window(self):
        _, con = self.open_window(self.ipc, "test.app")
        bindings = self.connect()
        bindings.subscribe("binding")
        with self.holding("logo", "super"):
            self.pointer(*self.over(con["id"]), "press", "middle", "release", "middle")
            event = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "maximize")
        self.assertEqual(event["container"]["id"], con["id"])
        self.assertTrue(event["container"]["maximized"])
        self.assertEqual(bindings.next_event(EVENT_BINDING, is_mouse)["binding"], {
            "command": "maximize toggle", "event_state_mask": ["Mod4"], "input_code": 0,
            "symbol": "button2", "input_type": "mouse"})

    def test_a_scroll_runs_its_binding(self):
        _, con = self.open_window(self.ipc, "test.app")
        bindings = self.connect()
        bindings.subscribe("binding")
        with self.holding("logo", "super"):
            self.pointer(*self.over(con["id"]), "scroll")
            event = bindings.next_event(EVENT_BINDING, is_mouse)
        self.assertEqual((event["binding"]["command"], event["binding"]["symbol"]),
                         ("nop scrolled down", "button5"))

    def test_a_scroll_ending_runs_no_binding(self):
        _, con = self.open_window(self.ipc, "test.app")
        bindings = self.connect()
        bindings.subscribe("binding")
        with self.holding("logo", "super"):
            self.pointer(*self.over(con["id"]), "stop", "scroll")
            event = bindings.next_event(EVENT_BINDING, is_mouse)
        self.assertEqual(event["binding"]["symbol"], "button5")

    def test_get_bindings_lists_them(self):
        mouse = [b for b in self.connect().request(GET_BINDINGS) if b["type"] == "mouse"]
        self.assertEqual([(b["keys"], b["command"]) for b in mouse], [
            ("Mod4+button2", "maximize toggle"), ("Mod4+button4", "nop scrolled up"),
            ("Mod4+button5", "nop scrolled down")])


class MouseConfigErrorsTest(MouseTest):
    CONFIG = HOLD

    def test_bad_mouse_lines_are_errors(self):
        for text, error in [
            ("bindsym Mod4+button2 nop\n",
             ":1: bindsym Mod4+button2: a button's binding needs --whole-window"),
            ("bindsym --whole-window Mod4+x nop\n",
             ":1: bindsym --whole-window Mod4+x: no button is called x"),
            ("bindsym --whole-window --release Mod4+button1 nop\n",
             ":1: bindsym --whole-window Mod4+button1: a button's binding takes no --release"),
            ("floating_modifier Hyper\n", ":1: floating_modifier: no modifier is called Hyper"),
            ("floating_modifier Mod4 sideways\n", ":1: floating_modifier takes a modifier"),
            ("mode m {\nfloating_modifier Mod4\n}\n", ":2: floating_modifier can't be in a mode"),
        ]:
            with self.subTest(text=text):
                self.assertIn(error, self.fails_to_load(text))


if __name__ == "__main__":
    unittest.main()
