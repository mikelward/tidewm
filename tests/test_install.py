"""What make install puts where: tidewm under its own name, so it never
stands in for a distro's dwl, and the portals its session picks."""

import configparser
import os
import subprocess
import tempfile
import unittest

from harness import ROOT


def make(*args):
    # Run on its own, not as part of the make that runs the tests.
    env = {k: v for k, v in os.environ.items() if k not in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL")}
    subprocess.run(["make", "-s", *args], cwd=ROOT, env=env, check=True,
                   stdout=subprocess.DEVNULL)


class InstallTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = tmp.name
        make("install", "DESTDIR=" + self.root, "PREFIX=/usr")

    def path(self, name):
        return os.path.join(self.root, "usr", name)

    def test_installs_the_binary_as_tidewm(self):
        self.assertTrue(os.access(self.path("bin/tidewm"), os.X_OK))
        self.assertFalse(os.path.exists(self.path("bin/dwl")))
        self.assertTrue(os.path.isfile(self.path("share/man/man1/tidewm.1")))

    def test_installs_no_session_entry(self):
        # tide's entry starts it, under uwsm.
        self.assertFalse(os.path.exists(self.path("share/wayland-sessions")))

    def test_portals_are_wlr_then_gtk(self):
        config = configparser.ConfigParser()
        read = config.read(self.path("share/xdg-desktop-portal/tidewm-portals.conf"))
        self.assertTrue(read)
        self.assertEqual(config["preferred"]["default"], "wlr;gtk")

    def test_uninstall_removes_what_install_put(self):
        make("uninstall", "DESTDIR=" + self.root, "PREFIX=/usr")
        left = [os.path.join(d, f) for d, _, files in os.walk(self.root) for f in files]
        self.assertEqual(left, [])


if __name__ == "__main__":
    unittest.main()
