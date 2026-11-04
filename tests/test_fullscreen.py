"""Fullscreen on open (tide's SPEC §6.3, §14.3): a window that asks for
fullscreen as it opens, a game or a video player with --fullscreen, opens
fullscreen once the focus guard lets it take focus. One the guard holds
opens unfocused and as it is, leaving the window you're in alone, even a
fullscreen one, and goes fullscreen once you go to it."""

import select
import unittest

from harness import EVENT_WINDOW, TIMEOUT, TidewmTest


class FullscreenHelpers(TidewmTest):
    """Helpers, with no tests of their own."""

    def setUp(self):
        super().setUp()
        self.ipc = self.connect()
        self.ipc.subscribe("window")

    def open(self, app_id, x11=False, fullscreen=False):
        """Opens a window, returning its id and its client."""
        if x11:
            self.need(self.wm.x11, "X11 windows need tidewm built with XWAYLAND")
            client = self.wm.open_x11_window(app_id, app_id, stdin=True, fullscreen=fullscreen)
            match = lambda e: (e["change"] == "new" and e["container"].get(
                "window_properties", {}).get("class") == app_id)
        else:
            client = self.wm.open_window(app_id, app_id, stdin=True, stdout=True,
                                         fullscreen=fullscreen)
            match = lambda e: e["change"] == "new" and e["container"]["app_id"] == app_id
        return self.ipc.next_event(EVENT_WINDOW, match)["container"]["id"], client

    def send(self, client, line):
        client.stdin.write(line.encode() + b"\n")
        client.stdin.flush()

    def told(self, client, expected):
        """The app's next line: what it was told, or "activated"."""
        self.assertTrue(select.select([client.stdout], [], [], TIMEOUT)[0], f"not {expected}")
        self.assertEqual(client.stdout.readline().decode().strip(), expected)

    def heard(self, client):
        """Waits until tidewm has had everything the app asked so far: an
        activation after it says when."""
        self.send(client, "activate")
        self.told(client, "activated")

    def fullscreen(self, con_id):
        return self.windows()[con_id]["fullscreen_mode"]

    def focused(self, con_id):
        return self.windows()[con_id]["focused"]

    def go_to(self, con_id):
        self.command(self.connect(), f"[con_id={con_id}] focus")

    def you_are_in(self, app_id, fullscreen=False):
        """Opens the window you're in, with a grant, for the guard."""
        self.command(self.connect(), f"grant {app_id}")
        window, client = self.open(app_id)
        if fullscreen:
            self.command(self.connect(), f"[con_id={window}] fullscreen enable")
        self.assertTrue(self.focused(window))
        return window


class OpenTest(FullscreenHelpers):
    def test_a_window_that_asks_before_it_maps_opens_fullscreen(self):
        window, client = self.open("test.video", fullscreen=True)
        self.told(client, "fullscreen")
        self.assertEqual(self.fullscreen(window), 1)

    def test_an_x11_window_fullscreen_as_it_maps_opens_fullscreen(self):
        window, _ = self.open("test.game", x11=True, fullscreen=True)
        self.assertEqual(self.fullscreen(window), 1)

    def test_focusing_a_window_behind_a_fullscreen_one_ends_that_fullscreen(self):
        a, _ = self.open("test.a")
        b, _ = self.open("test.b")
        self.go_to(a)
        self.command(self.connect(), "fullscreen enable")
        self.go_to(b)
        self.assertEqual((self.focused(b), self.fullscreen(a)), (True, 0))

    def test_a_fullscreen_windows_dialog_leaves_it_fullscreen(self):
        a, client = self.open("test.a")
        self.command(self.connect(), "fullscreen enable")
        self.send(client, "dialog")
        dialog = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                                     and e["container"]["app_id"] == "test.a.dialog")
        self.assertEqual((self.focused(dialog["container"]["id"]), self.fullscreen(a)), (True, 1))


class GuardTest(FullscreenHelpers):
    CONFIG = "focus_guard enable\n"

    def test_a_granted_window_opens_fullscreen(self):
        self.command(self.connect(), "grant test.video")
        window, client = self.open("test.video", fullscreen=True)
        self.told(client, "fullscreen")
        self.assertEqual((self.focused(window), self.fullscreen(window)), (True, 1))

    def test_a_held_window_goes_fullscreen_once_you_go_to_it(self):
        mine = self.you_are_in("test.mine")
        window, client = self.open("test.video", fullscreen=True)
        self.assertEqual((self.focused(mine), self.fullscreen(window)), (True, 0))
        self.go_to(window)
        self.told(client, "fullscreen")
        self.assertEqual(self.fullscreen(window), 1)

    def test_a_held_window_leaves_your_fullscreen_window_as_it_is(self):
        mine = self.you_are_in("test.mine", fullscreen=True)
        self.open("test.other")
        self.assertEqual((self.focused(mine), self.fullscreen(mine)), (True, 1))

    def test_a_held_windows_later_request_waits_for_you_too(self):
        mine = self.you_are_in("test.mine")
        window, client = self.open("test.video")
        self.send(client, "fullscreen")
        self.heard(client)
        self.assertEqual((self.focused(mine), self.fullscreen(window)), (True, 0))
        self.go_to(window)
        self.told(client, "fullscreen")
        self.assertEqual(self.fullscreen(window), 1)

    def test_a_held_window_that_takes_its_request_back_stays_as_it_is(self):
        self.you_are_in("test.mine")
        window, client = self.open("test.video", fullscreen=True)
        self.send(client, "unfullscreen")
        self.heard(client)
        self.go_to(window)
        self.assertEqual(self.fullscreen(window), 0)

    def test_going_to_a_held_window_ends_your_fullscreen(self):
        mine = self.you_are_in("test.mine", fullscreen=True)
        other, _ = self.open("test.other")
        self.go_to(other)
        self.assertEqual((self.focused(other), self.fullscreen(mine)), (True, 0))

    def test_the_fullscreen_command_sees_a_held_windows_request(self):
        self.you_are_in("test.mine")
        window, _ = self.open("test.video", fullscreen=True)
        # toggle takes the request back, a second puts it back, and disable
        # takes it back for good
        for how in ("toggle", "toggle", "disable"):
            self.command(self.connect(), f"[con_id={window}] fullscreen {how}")
        self.go_to(window)
        self.assertEqual(self.fullscreen(window), 0)

    def test_taking_a_held_windows_request_back_leaves_it_where_it_is(self):
        self.you_are_in("test.mine")
        window, _ = self.open("test.video")
        self.command(self.connect(), f"[con_id={window}] floating enable")
        before = self.windows()[window]["rect"]
        self.command(self.connect(), f"[con_id={window}] fullscreen enable")
        self.command(self.connect(), f"[con_id={window}] fullscreen disable")
        self.assertEqual(self.windows()[window]["rect"], before)

    def test_the_fullscreen_command_toggles_a_held_window_into_waiting(self):
        self.you_are_in("test.mine")
        window, _ = self.open("test.video")
        self.command(self.connect(), f"[con_id={window}] fullscreen toggle")
        self.assertEqual(self.fullscreen(window), 0)
        self.go_to(window)
        self.assertEqual(self.fullscreen(window), 1)

    def test_a_held_x11_window_waits_too(self):
        mine = self.you_are_in("test.mine")
        window, _ = self.open("test.game", x11=True, fullscreen=True)
        self.assertEqual((self.focused(mine), self.fullscreen(window)), (True, 0))
        self.go_to(window)
        self.assertEqual(self.fullscreen(window), 1)



class TwoOutputTest(FullscreenHelpers):
    CONFIG = "focus_guard enable\n"
    OUTPUTS = 2

    def test_a_held_windows_request_waits_through_its_output_going(self):
        self.command(self.connect(), "focus output HEADLESS-2")
        self.you_are_in("test.mine")
        window, _ = self.open("test.video", fullscreen=True)
        self.assertEqual(self.windows()[window]["output"], "HEADLESS-2")
        self.command(self.connect(), "output HEADLESS-2 disable")
        self.assertEqual((self.windows()[window]["output"], self.fullscreen(window)),
                         ("HEADLESS-1", 0))
        self.go_to(window)
        self.assertEqual(self.fullscreen(window), 1)


if __name__ == "__main__":
    unittest.main()
