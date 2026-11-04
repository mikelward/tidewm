"""The lock, as tide's SPEC §10 has it, over ext-session-lock: a locker
that dies leaves the session locked, with nothing reaching the windows
behind it, until a new locker takes the lock back or the unlock command
drops it.

A killed locker's hangup reaches tidewm before anything the test does
after it, as the kernel queues it when the locker exits and tidewm reads
its clients in the order they became ready."""

import os
import select
import subprocess
import unittest

from harness import (EVENT_TICK, EVENT_WINDOW, GET_OUTPUTS, ROOT, RUN_COMMAND, SEND_TICK,
                     TIMEOUT, TidewmTest)

POINTER = os.path.join(ROOT, "tests", "pointer")


class LockTest(TidewmTest):
    def setUp(self):
        super().setUp()
        self.ipc = self.connect()
        self.ipc.subscribe("window")

    def open(self):
        """Opens a window that says what reaches it from the pointer, and
        has the pointer, returning its id and its client."""
        client = self.wm.open_window("test.app", "test.app", stdout=True, scrolls=True)
        window = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new")["container"]
        # The headless seat has a pointer once a virtual one comes
        self.point(0, 0)
        self.line(client, (b"pointing",))
        return window, client

    def point(self, x, y, *actions):
        """Moves the pointer to x, y, then does the actions."""
        output, = self.connect().request(GET_OUTPUTS)
        size = [str(output["rect"]["width"]), str(output["rect"]["height"])]
        subprocess.run([POINTER, "move", str(x), str(y), *size, *actions], env=self.wm.env,
                       check=True, timeout=TIMEOUT)

    def over(self, window, *actions):
        """Moves the pointer over window, then does the actions."""
        self.point(window["rect"]["x"] + 50, window["rect"]["y"] + 50, *actions)

    def line(self, client, starts):
        """The client's next line that starts with one of starts."""
        while True:
            self.assertTrue(select.select([client.stdout], [], [], TIMEOUT)[0],
                            f"no {starts!r} from the window")
            line = client.stdout.readline()
            self.assertTrue(line, f"the window went before {starts!r}")
            if line.startswith(starts):
                return line

    def die(self, locker):
        locker.kill()
        locker.wait(TIMEOUT)

    def unlock_command(self):
        result, = self.connect().request(RUN_COMMAND, "unlock")
        return result

    def test_nothing_reaches_a_window_behind_a_dead_lock(self):
        window, client = self.open()
        self.die(self.lock())
        self.over(window, "click", "scroll")
        self.assertEqual(self.unlock_command(), {"success": True})
        self.over(window, "scroll", "up")
        # The first the window hears is the scroll after the unlock
        self.assertEqual(self.line(client, (b"button", b"scroll ")), b"scroll -15.00\n")

    def test_unlock_drops_a_dead_lock_and_focuses_the_window(self):
        window, _ = self.open()
        self.die(self.lock())
        self.assertEqual(self.unlock_command(), {"success": True})
        self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "focus"
                            and e["container"]["id"] == window["id"])
        self.assertTrue(self.windows()[window["id"]]["focused"])

    def test_unlock_fails_while_the_locker_holds_the_lock(self):
        window, client = self.open()
        locker = self.lock()
        result = self.unlock_command()
        self.assertFalse(result["success"], result)
        self.assertIn("only it may unlock", result["error"])
        # Still locked: the window hears nothing until the locker unlocks
        self.over(window, "scroll")
        self.unlock(locker)
        self.over(window, "scroll", "up")
        self.assertEqual(self.line(client, (b"button", b"scroll ")), b"scroll -15.00\n")

    def test_unlock_fails_with_nothing_locked(self):
        result = self.unlock_command()
        self.assertEqual(result, {"success": False, "error": "Nothing is locked"})

    def test_a_new_locker_takes_a_dead_lock_with_no_window_focused_between(self):
        window, _ = self.open()
        self.die(self.lock())
        ipc = self.connect()
        ipc.subscribe("window", "tick")
        ipc.next_event(EVENT_TICK)  # the one subscribing sends
        locker = self.lock()
        self.connect().request(SEND_TICK, "relocked")
        # No window took focus between the locks: the tick comes first
        kind, event = ipc.recv()
        self.assertEqual((kind, event["payload"]), (EVENT_TICK, "relocked"))
        self.unlock(locker)
        self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "focus"
                            and e["container"]["id"] == window["id"])


if __name__ == "__main__":
    unittest.main()
