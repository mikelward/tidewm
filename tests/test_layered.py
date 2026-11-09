"""tide's tidewm config with conf's on top, as a tide (tidewm) session
loads them: conf's ~/.config/tidewm/config includes tide's tide.conf and
binds its own commands over it. Each repo tests its own file; this loads
the two together, so a change to either that the other can't take shows
here, not as a session that comes up without its keys.

A machine's tide and conf are often behind: setup builds tidewm's latest
main, but the configs are whatever that machine last pulled. So this
loads the pair as they are now, and as they were a week and a month ago,
each pair from the same day, as a machine set up that day has them. This
tidewm has to take every one: a config that worked once keeps working.

It needs full clones of both, named by TIDE_CHECKOUT and CONF_CHECKOUT,
as the layered workflow gives it (.github/workflows/layered.yml), and is
skipped without them."""

import os
import subprocess
import unittest

from harness import GET_CONFIG, RUN_COMMAND, TidewmTest

TIDE = os.environ.get("TIDE_CHECKOUT")
CONF = os.environ.get("CONF_CHECKOUT")

TIDE_CONF = "tidewm/tide.conf"
CONF_CONFIG = "config/tidewm/config"

# How far back each pair is taken from: now, a week ago and a month ago.
AGES = ["now", "7 days ago", "30 days ago"]


def git(checkout, *args):
    return subprocess.run(["git", "-C", checkout, *args], check=True,
                          capture_output=True, text=True).stdout


def commit_at(checkout, age):
    """The checkout's commit as of age, or None if it has none that old."""
    if age == "now":
        return git(checkout, "rev-parse", "HEAD").strip()
    return git(checkout, "rev-list", "-1", f"--before={age}", "HEAD").strip() or None


def file_at(checkout, commit, path):
    """The file at commit, or None where it didn't exist yet."""
    result = subprocess.run(["git", "-C", checkout, "show", f"{commit}:{path}"],
                            capture_output=True, text=True)
    return result.stdout if result.returncode == 0 else None


@unittest.skipUnless(TIDE and CONF, "needs TIDE_CHECKOUT and CONF_CHECKOUT")
class LayeredConfigTest(TidewmTest):
    CONFIG = ""

    def load(self, tide_conf, conf_config):
        """Loads conf's config over tide's, as a session would, and returns
        the load's result and GET_CONFIG's reply."""
        directory = os.path.dirname(self.wm.config_path)
        with open(os.path.join(directory, "tide.conf"), "w") as f:
            f.write(tide_conf)
        self.wm.write_config(conf_config)
        result, = self.connect().request(RUN_COMMAND, "reload")
        return result, self.connect().request(GET_CONFIG)

    def test_confs_config_over_tides_loads_without_errors(self):
        loaded = 0
        for age in AGES:
            tide_commit, conf_commit = commit_at(TIDE, age), commit_at(CONF, age)
            if not tide_commit or not conf_commit:
                continue
            tide_conf = file_at(TIDE, tide_commit, TIDE_CONF)
            conf_config = file_at(CONF, conf_commit, CONF_CONFIG)
            # Before the tidewm session, there was nothing to load.
            if tide_conf is None or conf_config is None:
                continue
            with self.subTest(age=age, tide=tide_commit[:12], conf=conf_commit[:12]):
                result, config = self.load(tide_conf, conf_config)
                self.assertTrue(result["success"], result.get("error"))
                self.assertEqual(config["errors"], [])
                self.assertIn("tide.conf", [os.path.basename(c["path"])
                                            for c in config["included_configs"]])
                loaded += 1
        self.assertGreater(loaded, 0, "no pair of tide's and conf's configs was found to load")


if __name__ == "__main__":
    unittest.main()
