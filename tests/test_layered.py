"""tide's tidewm config with conf's on top, as a tide (tidewm) session
loads them: conf's ~/.config/tidewm/config includes tide's tide.conf and
binds its own commands over it. Each repo tests its own file; this loads
the two together, so a change to either that the other can't take shows
here, not as a session that comes up without its keys.

It needs checkouts of both, named by TIDE_CHECKOUT and CONF_CHECKOUT, as
the layered workflow gives it (.github/workflows/layered.yml), and is
skipped without them."""

import os
import shutil
import unittest

from harness import GET_CONFIG, RUN_COMMAND, TidewmTest

TIDE = os.environ.get("TIDE_CHECKOUT")
CONF = os.environ.get("CONF_CHECKOUT")


@unittest.skipUnless(TIDE and CONF, "needs TIDE_CHECKOUT and CONF_CHECKOUT")
class LayeredConfigTest(TidewmTest):
    CONFIG = ""

    def test_confs_config_over_tides_loads_without_errors(self):
        directory = os.path.dirname(self.wm.config_path)
        shutil.copy(os.path.join(TIDE, "tidewm", "tide.conf"), os.path.join(directory, "tide.conf"))
        with open(os.path.join(CONF, "config", "tidewm", "config")) as f:
            self.wm.write_config(f.read())
        result, = self.connect().request(RUN_COMMAND, "reload")
        self.assertTrue(result["success"], result.get("error"))
        config = self.connect().request(GET_CONFIG)
        self.assertEqual(config["errors"], [])
        self.assertIn("tide.conf", [os.path.basename(c["path"]) for c in config["included_configs"]])


if __name__ == "__main__":
    unittest.main()
