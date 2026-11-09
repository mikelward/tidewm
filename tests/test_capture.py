"""Capture (tide's SPEC §12): the IPC's capture event, tidewm's own, says
when an ext-image-copy-capture session starts and stops, as a screen
share's or a screenshot's, and whether it's of a window, by its con_id,
or of an output; and that a window can be captured, as grim -T and tide's
overview do, shown or on a workspace not shown."""

import os
import select
import subprocess
import unittest

from harness import EVENT_CAPTURE, EVENT_WINDOW, GET_OUTPUTS, ROOT, TIMEOUT, TidewmTest

CAPTURE = os.path.join(ROOT, "tests", "capture")
POINTER = os.path.join(ROOT, "tests", "pointer")


class CaptureTest(TidewmTest):
    def setUp(self):
        super().setUp()
        self.ipc = self.connect()
        self.ipc.subscribe("window", "capture")

    def open(self, app_id):
        """Opens a window, returning its id and its client."""
        client = self.wm.open_window(app_id, app_id)
        window = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                                     and e["container"]["app_id"] == app_id)["container"]
        return window["id"], client

    def capture(self, *what):
        """Starts tests/capture's session of what, returning it once
        tidewm has it."""
        share = subprocess.Popen([CAPTURE, *what], env=self.wm.env, stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE)
        self.wm.clients.append(share)
        self.addCleanup(share.stdout.close)
        self.assertEqual(self.line(share), b"capturing\n")
        return share

    def stop(self, share):
        share.stdin.write(b"stop\n")
        share.stdin.flush()
        self.assertEqual(self.line(share), b"stopped\n")

    def line(self, process):
        self.assertTrue(select.select([process.stdout], [], [], TIMEOUT)[0], "it said nothing")
        return process.stdout.readline()

    def event(self):
        return self.ipc.next_event(EVENT_CAPTURE)

    def test_a_window_capture_says_which_window(self):
        window, _ = self.open("test.app")
        share = self.capture("window", "test.app")
        self.assertEqual(self.event(), {"change": "start", "source": "window", "con_id": window})
        self.stop(share)
        self.assertEqual(self.event(), {"change": "stop", "source": "window", "con_id": window})

    def test_an_output_capture_names_no_window(self):
        share = self.capture("output")
        self.assertEqual(self.event(), {"change": "start", "source": "output", "con_id": None})
        self.stop(share)
        self.assertEqual(self.event(), {"change": "stop", "source": "output", "con_id": None})

    def test_a_stream_of_just_the_cursor_isnt_a_capture(self):
        window, _ = self.open("test.app")
        # The headless seat has a pointer once a virtual one comes
        output, = self.connect().request(GET_OUTPUTS)
        subprocess.run([POINTER, "move", "0", "0", str(output["rect"]["width"]),
                        str(output["rect"]["height"])], env=self.wm.env, check=True,
                       timeout=TIMEOUT)
        self.capture("cursor")
        self.capture("window", "test.app")
        # The window's is the first capture
        self.assertEqual(self.event(), {"change": "start", "source": "window", "con_id": window})

    def test_a_capture_stops_when_its_client_goes(self):
        share = self.capture("output")
        self.event()
        share.kill()
        share.wait(TIMEOUT)
        self.assertEqual(self.event()["change"], "stop")

    def test_a_window_capture_stops_when_the_window_closes(self):
        window, client = self.open("test.app")
        self.capture("window", "test.app")
        self.event()
        client.kill()
        client.wait(TIMEOUT)
        self.assertEqual(self.event(), {"change": "stop", "source": "window", "con_id": window})

    def shot(self, app_id):
        """tests/capture's one frame of the window with app_id: its size
        and its middle pixel's color, as "WxH RRGGBB"."""
        return subprocess.run([CAPTURE, "shot", app_id], env=self.wm.env, check=True,
                              capture_output=True, timeout=TIMEOUT).stdout.decode().strip()

    def test_a_window_can_be_captured(self):
        self.open("test.app")
        # tests/client fills its window with 336699
        self.assertRegex(self.shot("test.app"), r"^[1-9][0-9]*x[1-9][0-9]* 336699$")

    def test_a_window_on_a_workspace_not_shown_can_be_captured(self):
        self.open("test.app")
        self.command(self.connect(), "workspace two")
        self.assertRegex(self.shot("test.app"), r"^[1-9][0-9]*x[1-9][0-9]* 336699$")


if __name__ == "__main__":
    unittest.main()
