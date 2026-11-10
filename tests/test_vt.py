"""Ctrl+Alt+F1…F12 switch VT whatever the config file binds, so a file that
lost its keys leaves a way out. The headless backend has no VTs to switch,
so these check the keys are tidewm's: a window never sees them, and a
binding of the file's on them still runs."""

import subprocess
import unittest

from harness import EVENT_BINDING
from test_keep_open import KEYBOARD, KeepOpenHelpers


class VtKeysTest(KeepOpenHelpers):
    # A file with a binding, so config.h's keys, its VT keys among them, are off
    CONFIG = ("bindsym Mod4+x workspace x\nbindsym Ctrl+Mod1+F3 workspace three\n"
              "bindsym --release Ctrl+Mod1+F4 workspace four\n")

    def setUp(self):
        super().setUp()
        # The headless backend has no keyboard: the tests' is the seat's
        keyboard = subprocess.Popen([KEYBOARD, "0"], env=dict(self.wm.env, XKB_LAYOUT="us"),
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.wm.clients.append(keyboard)
        self.addCleanup(keyboard.stdout.close)
        self.addCleanup(keyboard.stdin.close)
        self.assertEqual(self.line(keyboard), b"switched\n")

    def ctrl_alt(self, key):
        self.press("-M", "ctrl", "-M", "alt", "-k", key, "-m", "alt", "-m", "ctrl")

    def test_a_vt_key_the_file_doesnt_bind_never_reaches_a_window(self):
        client, _ = self.open("test.typist", keys=True)
        self.ctrl_alt("F2")
        # The next key is the first the window hears of
        self.press("-k", "a")
        self.assertEqual(self.line(client), b"key a\n")

    def test_a_binding_of_the_files_on_a_vt_key_still_runs(self):
        bindings = self.connect()
        bindings.subscribe("binding")
        self.ctrl_alt("F3")
        self.assertEqual(bindings.next_event(EVENT_BINDING)["binding"]["command"], "workspace three")

    def test_a_release_binding_of_the_files_on_a_vt_key_keeps_it(self):
        client, _ = self.open("test.typist", keys=True)
        bindings = self.connect()
        bindings.subscribe("binding")
        self.ctrl_alt("F4")
        # A release binding's key reaches the window too, as in sway
        self.assertTrue(self.line(client).startswith(b"key ctrl+alt+"))
        self.assertEqual(bindings.next_event(EVENT_BINDING)["binding"]["command"], "workspace four")

    def test_other_ctrl_alt_keys_reach_the_window(self):
        client, _ = self.open("test.typist", keys=True)
        self.ctrl_alt("a")
        self.assertEqual(self.line(client), b"key ctrl+alt+a\n")


if __name__ == "__main__":
    unittest.main()
