"""Layer surfaces and the keyboard, as tide's SPEC §14.1 has it: one with
on-demand focus, such as a notification's reply field, takes the keyboard
when you click it, not when the pointer crosses it, and keeps it until you
move it, by a click elsewhere, a key binding or a command. A window
opening or closing, or the pointer crossing one, doesn't take it.

A notification center's focus grab, over hyprland-focus-grab-v1, as
Quickshell's HyprlandFocusGrab makes one (tide's SPEC §9), holds the
keyboard and pointer the same way, and ends on a click outside it, which
goes no further."""

import os
import select
import subprocess
import unittest

from harness import (EVENT_WINDOW, GET_OUTPUTS, ROOT, TIMEOUT, TidewmTest)

POINTER = os.path.join(ROOT, "tests", "pointer")
LAUNCHER = os.path.join(ROOT, "tests", "launcher")


class LayerHelpers(TidewmTest):
    """Helpers, with no tests of their own."""
    CONFIG = "bindsym Mod1+x focus next\n"

    def setUp(self):
        super().setUp()
        # The headless seat has a keyboard once a virtual one comes, and
        # keeps it; the notification takes its own then
        self.press("-k", "Escape")
        self.ipc = self.connect()
        self.ipc.subscribe("window")

    def open(self, app_id, **kwargs):
        """Opens a window, returning its container and its client."""
        client = self.wm.open_window(app_id, app_id, **kwargs)
        window = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                                     and e["container"]["app_id"] == app_id)["container"]
        return window, client

    def notification(self, kind="notification"):
        """Opens tests/launcher's notification, in the top right corner,
        which takes the keyboard when it's clicked, or its center."""
        # Unbuffered, so a line's select isn't fooled by the one before
        # it having read both
        note = subprocess.Popen([LAUNCHER, kind], env=self.wm.env, bufsize=0,
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.wm.clients.append(note)
        self.addCleanup(note.stdout.close)
        self.assertEqual(self.line(note), b"open\n")
        return note

    def line(self, process):
        self.assertTrue(select.select([process.stdout], [], [], TIMEOUT)[0], "it said nothing")
        return process.stdout.readline()

    def point(self, how, x, y, *actions):
        """Moves the pointer to x, y, how tests/pointer does ("move" or
        "cross"), then does the actions."""
        output, = self.connect().request(GET_OUTPUTS)
        size = [str(output["rect"]["width"]), str(output["rect"]["height"])]
        subprocess.run([POINTER, how, str(x), str(y), *size, *actions], env=self.wm.env,
                       check=True, timeout=TIMEOUT)

    def pointer(self, *actions):
        """Does tests/pointer's actions where the pointer is."""
        subprocess.run([POINTER, *actions], env=self.wm.env, check=True, timeout=TIMEOUT)

    def at_notification(self, how, *actions):
        output, = self.connect().request(GET_OUTPUTS)
        self.point(how, output["rect"]["width"] - 100, 50, *actions)

    def over(self, window, how, *actions):
        """Moves the pointer to the middle of window, clear of the
        notification, then does the actions."""
        rect = window["rect"]
        self.point(how, rect["x"] + rect["width"] // 2, rect["y"] + rect["height"] // 2, *actions)

    def typing_in(self, note):
        """Clicks the notification's reply field, which takes the keyboard."""
        self.at_notification("move", "click")
        self.assertEqual(self.line(note), b"focused\n")
        self.assertEqual(self.focused(), [])

    def typed(self, note):
        """Types a key, and asserts the notification has it, as it would
        have lost the keyboard first otherwise."""
        self.press("-k", "a")
        self.assertEqual(self.line(note), b"key\n")

    def focused(self):
        return [i for i, w in self.windows().items() if w["focused"]]


class OnDemandTest(LayerHelpers):
    def test_the_pointer_crossing_a_notification_leaves_the_window_the_keyboard(self):
        window, _ = self.open("test.first")
        self.notification()
        self.over(window, "cross")
        self.at_notification("cross")
        self.assertEqual(self.focused(), [window["id"]])

    def test_a_click_gives_a_notification_the_keyboard(self):
        self.open("test.first")
        note = self.notification()
        self.typing_in(note)
        self.typed(note)

    def test_a_window_opening_doesnt_take_it(self):
        self.open("test.first")
        note = self.notification()
        self.typing_in(note)
        self.open("test.second")
        self.assertEqual(self.focused(), [])
        self.typed(note)

    def test_the_pointer_crossing_a_window_doesnt_take_it(self):
        window, _ = self.open("test.first")
        note = self.notification()
        self.typing_in(note)
        self.over(window, "cross")
        self.assertEqual(self.focused(), [])
        self.typed(note)

    def test_the_last_window_closing_doesnt_take_it(self):
        _, client = self.open("test.first")
        note = self.notification()
        self.typing_in(note)
        client.kill()
        client.wait(TIMEOUT)
        self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "close")
        self.typed(note)

    def test_a_click_on_a_window_takes_it(self):
        window, _ = self.open("test.first")
        note = self.notification()
        self.typing_in(note)
        self.over(window, "move", "click")
        self.assertEqual(self.line(note), b"unfocused\n")
        self.assertEqual(self.focused(), [window["id"]])

    def test_a_command_takes_it(self):
        window, _ = self.open("test.first")
        note = self.notification()
        self.typing_in(note)
        self.command(self.connect(), f"[con_id={window['id']}] focus")
        self.assertEqual(self.line(note), b"unfocused\n")
        self.assertEqual(self.focused(), [window["id"]])

    def test_a_key_binding_takes_it(self):
        window, _ = self.open("test.first")
        note = self.notification()
        self.typing_in(note)
        self.press("-M", "alt", "-k", "x", "-m", "alt")
        self.assertEqual(self.line(note), b"unfocused\n")
        self.assertEqual(self.focused(), [window["id"]])

    def test_the_launcher_takes_it(self):
        self.open("test.first")
        note = self.notification()
        self.typing_in(note)
        launcher = subprocess.Popen([LAUNCHER], env=self.wm.env, stdin=subprocess.PIPE,
                                    stdout=subprocess.PIPE)
        self.wm.clients.append(launcher)
        self.addCleanup(launcher.stdout.close)
        self.assertEqual(self.line(launcher), b"open\n")
        self.assertEqual(self.line(note), b"unfocused\n")

    def test_the_lock_takes_it(self):
        self.open("test.first")
        note = self.notification()
        self.typing_in(note)
        self.lock()
        self.assertEqual(self.line(note), b"unfocused\n")


class GrabTest(LayerHelpers):
    def center(self):
        """Opens tests/launcher's notification center, whose focus grab
        gives it the keyboard."""
        center = self.notification("center")
        self.assertEqual(self.line(center), b"focused\n")
        self.assertEqual(self.line(center), b"grabbing\n")
        return center

    def holds(self, center):
        """Asserts the grab still holds: the center hears nothing before
        its pong."""
        center.stdin.write(b"ping\n")
        center.stdin.flush()
        self.assertEqual(self.line(center), b"pong\n")

    def test_a_grab_takes_the_keyboard(self):
        self.open("test.first")
        center = self.center()
        self.assertEqual(self.focused(), [])
        self.typed(center)

    def test_a_grab_of_a_menu_alone_gives_the_menu_the_keyboard(self):
        self.open("test.first")
        menu = self.notification("menu")
        self.assertEqual(self.line(menu), b"focused\n")
        self.assertEqual(self.line(menu), b"grabbing\n")
        self.typed(menu)

    def test_a_grab_takes_the_pointer_from_the_window_under_it(self):
        window, client = self.open("test.first", stdout=True, scrolls=True)
        # The headless seat has a pointer once a virtual one comes, on the
        # border here, so the window's own pointer sees it enter after
        self.point("move", 0, 0)
        while self.line(client) != b"pointing\n":
            pass
        self.over(window, "move")
        center = self.center()
        self.pointer("scroll")  # still over the window
        self.pointer("click")
        self.assertEqual(self.line(center), b"cleared\n")
        self.pointer("scroll", "up")
        # The first the window hears is the scroll after the grab
        line = self.line(client)
        while not line.startswith((b"button", b"scroll ")):
            line = self.line(client)
        self.assertEqual(line, b"scroll -15.00\n")

    def test_a_window_opening_leaves_a_grabbed_windows_menu_open(self):
        _, client = self.open("test.menu", stdin=True, stdout=True)
        for command, said in ((b"menu\n", b"menu\n"), (b"grab\n", b"grabbing\n")):
            client.stdin.write(command)
            client.stdin.flush()
            self.assertEqual(self.line(client), said)
        self.open("test.second")
        client.stdin.write(b"ping\n")
        client.stdin.flush()
        # Not "menu closed" first
        self.assertEqual(self.line(client), b"pong\n")

    def test_a_click_in_a_grabbed_menu_keeps_the_grab(self):
        self.open("test.first")
        menu = self.notification("menu")
        self.assertEqual(self.line(menu), b"focused\n")
        self.assertEqual(self.line(menu), b"grabbing\n")
        # The menu is in the notification's top left corner
        output, = self.connect().request(GET_OUTPUTS)
        self.point("move", output["rect"]["width"] - 175, 25, "click")
        self.holds(menu)
        self.typed(menu)

    def test_the_grab_going_gives_the_pointer_back(self):
        window, client = self.open("test.first", stdout=True, scrolls=True)
        self.point("move", 0, 0)
        while self.line(client) != b"pointing\n":
            pass
        self.over(window, "move")
        center = self.center()
        center.stdin.write(b"ungrab\n")
        center.stdin.flush()
        self.assertEqual(self.line(center), b"ungrabbed\n")
        self.pointer("scroll", "up")  # still over the window
        line = self.line(client)
        while not line.startswith((b"button", b"scroll ")):
            line = self.line(client)
        self.assertEqual(line, b"scroll -15.00\n")

    def test_a_window_opening_leaves_the_grabbing_window_the_one_kill_closes(self):
        first, client = self.open("test.first", stdin=True, stdout=True)
        client.stdin.write(b"grab\n")
        client.stdin.flush()
        self.assertEqual(self.line(client), b"grabbing\n")
        self.open("test.second")
        self.command(self.connect(), "kill")
        close = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "close")
        self.assertEqual(close["container"]["id"], first["id"])

    def test_a_windows_grab_of_its_own_menu_leaves_the_menu_open(self):
        _, client = self.open("test.menu", stdin=True, stdout=True)
        for command, said in ((b"menu\n", b"menu\n"), (b"grab menu\n", b"grabbing\n")):
            client.stdin.write(command)
            client.stdin.flush()
            # Not "menu closed" before "grabbing"
            self.assertEqual(self.line(client), said)

    def test_the_lock_ends_the_launchers_own_grab(self):
        launcher = self.notification("grab")
        self.assertEqual(self.line(launcher), b"grabbing\n")
        self.lock()
        self.assertEqual(self.line(launcher), b"cleared\n")

    def test_a_grab_of_a_window_before_it_maps_waits_for_it(self):
        window, client = self.open("test.early", stdout=True, grab_first=True)
        self.assertEqual(self.line(client), b"grabbing\n")
        self.assertEqual(self.focused(), [window["id"]])

    def test_a_grab_of_a_center_before_it_maps_gives_it_the_keyboard_as_it_does(self):
        self.open("test.first")
        early = subprocess.Popen([LAUNCHER, "early"], env=self.wm.env, bufsize=0,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.wm.clients.append(early)
        self.addCleanup(early.stdout.close)
        self.assertEqual(self.line(early), b"focused\n")
        self.assertEqual(self.line(early), b"open\n")
        self.assertEqual(self.focused(), [])

    def test_a_grab_of_a_menu_before_it_maps_gives_it_the_keyboard_as_it_does(self):
        self.open("test.first")
        menu = self.notification("early-menu")
        self.assertEqual(self.line(menu), b"focused\n")
        self.assertEqual(self.line(menu), b"grabbing\n")
        self.typed(menu)

    def test_the_focused_surface_going_gives_the_keyboard_to_another_of_the_grabs(self):
        self.open("test.first")
        pair = self.notification("pair")
        self.assertEqual(self.line(pair), b"focused\n")
        self.assertEqual(self.line(pair), b"grabbing\n")
        pair.stdin.write(b"close first\n")
        pair.stdin.flush()
        self.assertEqual(self.line(pair), b"unfocused\n")
        # The second has it, before the first is gone
        self.assertEqual(self.line(pair), b"focused\n")
        self.assertEqual(self.line(pair), b"closed first\n")
        self.assertEqual(self.focused(), [])
        self.typed(pair)

    def test_the_last_grabbed_surface_going_gives_the_keyboard_back(self):
        window, _ = self.open("test.first")
        center = self.center()
        # The center's surface goes, but not its grab
        center.stdin.write(b"close first\n")
        center.stdin.flush()
        self.assertEqual(self.line(center), b"unfocused\n")
        self.assertEqual(self.line(center), b"closed first\n")
        self.assertEqual(self.focused(), [window["id"]])

    def test_a_grabbed_menu_going_gives_the_keyboard_back(self):
        window, _ = self.open("test.first")
        menu = self.notification("menu")
        self.assertEqual(self.line(menu), b"focused\n")
        self.assertEqual(self.line(menu), b"grabbing\n")
        # The menu goes, but not the center or its grab
        menu.stdin.write(b"close menu\n")
        menu.stdin.flush()
        self.assertEqual(self.line(menu), b"unfocused\n")
        self.assertEqual(self.line(menu), b"closed menu\n")
        self.assertEqual(self.focused(), [window["id"]])

    def test_a_grab_cant_start_while_the_launcher_has_the_keyboard(self):
        launcher = subprocess.Popen([LAUNCHER], env=self.wm.env, stdin=subprocess.PIPE,
                                    stdout=subprocess.PIPE)
        self.wm.clients.append(launcher)
        self.addCleanup(launcher.stdout.close)
        self.assertEqual(self.line(launcher), b"open\n")
        center = self.notification("center")
        # Not "focused": the launcher keeps the keyboard
        self.assertEqual(self.line(center), b"cleared\n")
        self.assertEqual(self.line(center), b"grabbing\n")

    def test_a_grab_cant_start_under_the_lock(self):
        self.lock()
        center = self.notification("center")
        self.assertEqual(self.line(center), b"cleared\n")
        self.assertEqual(self.line(center), b"grabbing\n")

    def test_a_click_outside_ends_it_and_goes_no_further(self):
        window, client = self.open("test.first", stdout=True, scrolls=True)
        # The headless seat has a pointer once a virtual one comes
        self.point("move", 0, 0)
        while self.line(client) != b"pointing\n":
            pass
        center = self.center()
        self.over(window, "move", "click")
        self.assertEqual(self.line(center), b"cleared\n")
        self.over(window, "move", "scroll", "up")
        # The first the window hears is the scroll after the grab
        line = self.line(client)
        while not line.startswith((b"button", b"scroll ")):
            line = self.line(client)
        self.assertEqual(line, b"scroll -15.00\n")

    def test_a_click_inside_keeps_it(self):
        self.open("test.first")
        center = self.center()
        self.at_notification("move", "click")
        self.holds(center)

    def test_the_pointer_crossing_a_window_keeps_it(self):
        window, _ = self.open("test.first")
        center = self.center()
        self.over(window, "cross")
        self.holds(center)
        self.assertEqual(self.focused(), [])

    def test_a_window_opening_doesnt_take_the_keyboard(self):
        self.open("test.first")
        center = self.center()
        self.open("test.second")
        self.holds(center)
        self.assertEqual(self.focused(), [])

    def test_a_key_binding_ends_it(self):
        window, _ = self.open("test.first")
        center = self.center()
        self.press("-M", "alt", "-k", "x", "-m", "alt")
        self.assertEqual(self.line(center), b"cleared\n")
        self.assertEqual(self.line(center), b"unfocused\n")
        self.assertEqual(self.focused(), [window["id"]])

    def test_the_launcher_ends_it(self):
        self.open("test.first")
        center = self.center()
        launcher = subprocess.Popen([LAUNCHER], env=self.wm.env, stdin=subprocess.PIPE,
                                    stdout=subprocess.PIPE)
        self.wm.clients.append(launcher)
        self.addCleanup(launcher.stdout.close)
        self.assertEqual(self.line(launcher), b"open\n")
        self.assertEqual(self.line(center), b"cleared\n")
        self.assertEqual(self.line(center), b"unfocused\n")

    def test_the_lock_ends_it(self):
        self.open("test.first")
        center = self.center()
        self.lock()
        self.assertEqual(self.line(center), b"cleared\n")
        self.assertEqual(self.line(center), b"unfocused\n")


if __name__ == "__main__":
    unittest.main()
