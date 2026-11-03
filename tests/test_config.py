"""tidewm's config file, in sway's syntax: bindsym and set, read at startup
and again by reload (docs/config.md)."""

import os
import select
import shlex
import unittest

from harness import (EVENT_MODE, EVENT_WORKSPACE, GET_BINDING_MODES, GET_BINDING_STATE,
                     GET_CONFIG, GET_VERSION, RUN_COMMAND, TIMEOUT, TidewmTest)

CONFIG = """# Lines like this, and blank ones, are skipped

set $mod Mod1
bindsym $mod+x workspace keyed
bindsym Mod1+Shift+y workspace one; workspace two
"""


class KeysTest(TidewmTest):
    """Pressing keys and seeing which workspace they show."""

    def shown(self, ipc):
        """The workspace the next switch shows; ipc must be subscribed to
        workspace events."""
        return ipc.next_event(EVENT_WORKSPACE, lambda e: e["change"] == "focus")["current"]["name"]

    def alt(self, key, shift=False):
        """Presses Alt and key, with Shift if shift."""
        mods = ["alt", "shift"] if shift else ["alt"]
        self.press(*[a for m in mods for a in ("-M", m)], "-k", key,
                   *[a for m in reversed(mods) for a in ("-m", m)])

    def workspaces_ipc(self):
        ipc = self.connect()
        ipc.subscribe("workspace")
        return ipc

    def fails(self, command):
        result, = self.connect().request(RUN_COMMAND, command)
        self.assertFalse(result["success"], f"{command!r}: {result}")
        return result


class ConfigTest(KeysTest):
    CONFIG = CONFIG

    def test_a_binding_runs_its_command(self):
        ipc = self.workspaces_ipc()
        self.alt("x")
        self.assertEqual(self.shown(ipc), "keyed")

    def test_a_binding_runs_each_of_its_commands(self):
        ipc = self.workspaces_ipc()
        self.alt("y", shift=True)
        self.assertEqual([self.shown(ipc), self.shown(ipc)], ["one", "two"])

    def test_the_file_replaces_config_h_keys(self):
        ipc = self.workspaces_ipc()
        self.alt("2")  # config.h's Alt-2 would show workspace 2
        self.alt("x")
        self.assertEqual(self.shown(ipc), "keyed")

    def test_get_config_shows_the_file(self):
        config = self.connect().request(GET_CONFIG)
        self.assertEqual(config, {"config": CONFIG, "path": self.wm.config_path, "errors": []})
        version = self.connect().request(GET_VERSION)
        self.assertEqual(version["loaded_config_file_name"], self.wm.config_path)

    def test_reload_reads_the_file_again(self):
        self.wm.write_config("bindsym Mod1+x workspace again\n")
        self.command(self.connect(), "reload")
        ipc = self.workspaces_ipc()
        self.alt("x")
        self.assertEqual(self.shown(ipc), "again")

    def test_a_file_with_an_error_is_kept_out_and_the_old_one_stays(self):
        self.wm.write_config("bindsym Mod1+x workspace again\nfrobnicate now\n")
        error = f"{self.wm.config_path}:2: no setting is called frobnicate"
        self.assertEqual(self.fails("reload")["error"], error)
        ipc = self.workspaces_ipc()
        self.alt("x")
        self.assertEqual(self.shown(ipc), "keyed")
        config = self.connect().request(GET_CONFIG)
        self.assertEqual((config["config"], config["errors"]), (CONFIG, [error]))

    def test_a_modifier_or_key_tidewm_doesnt_know_is_an_error(self):
        self.wm.write_config("bindsym Hyper+x nop\nbindsym Mod1+NoSuchKey nop\n")
        error = self.fails("reload")["error"]
        self.assertIn(":1: bindsym Hyper+x: no modifier or key is called Hyper", error)
        self.assertIn(":2: bindsym Mod1+NoSuchKey: no modifier or key is called NoSuchKey", error)

    def test_a_binding_needs_a_command(self):
        self.wm.write_config("bindsym Mod1+x\n")
        self.assertIn("bindsym needs keys and a command", self.fails("reload")["error"])

    def test_release_and_locked_bindings_arent_supported_yet(self):
        self.wm.write_config("bindsym --release Mod1+x nop\n")
        self.assertIn("bindsym --release isn't supported yet", self.fails("reload")["error"])

    def test_set_needs_a_name(self):
        self.wm.write_config("set mod Mod1\n")
        self.assertIn(":1: set needs a $name", self.fails("reload")["error"])

    def test_a_later_binding_of_the_same_keys_wins(self):
        # A letter's case doesn't matter: X is the x key
        self.wm.write_config("bindsym Mod1+x workspace first\nbindsym Mod1+X workspace second\n")
        self.command(self.connect(), "reload")
        ipc = self.workspaces_ipc()
        self.alt("x")
        self.assertEqual(self.shown(ipc), "second")

    def test_lock_is_ignored_so_a_binding_naming_it_replaces_one_that_doesnt(self):
        self.wm.write_config("bindsym Mod1+x workspace first\nbindsym Lock+Mod1+x workspace second\n")
        self.command(self.connect(), "reload")
        ipc = self.workspaces_ipc()
        self.alt("x")
        self.assertEqual(self.shown(ipc), "second")

    def test_an_escaped_space_at_the_end_of_a_line_stays(self):
        fifo = os.path.join(self.wm.dir.name, "out")
        os.mkfifo(fifo)
        fd = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
        self.addCleanup(os.close, fd)
        # The line's own unescaped space and tab still go
        self.wm.write_config(f"bindsym Mod1+x exec exec > {shlex.quote(fifo)}; "
                             "printf '<%s>' foo\\  \t\n")
        self.command(self.connect(), "reload")
        self.alt("x")
        self.assertTrue(select.select([fd], [], [], TIMEOUT)[0], "the binding's exec didn't run")
        output = b""
        while chunk := os.read(fd, 100):  # to the end, when the shell exits
            output += chunk
            select.select([fd], [], [], TIMEOUT)
        self.assertEqual(output, b"<foo >")

    def test_the_longest_variable_name_wins(self):
        self.wm.write_config("set $w workspace short\nset $wide workspace long\n"
                             "bindsym Mod1+x $wide\n")
        self.command(self.connect(), "reload")
        ipc = self.workspaces_ipc()
        self.alt("x")
        self.assertEqual(self.shown(ipc), "long")

    def test_reload_without_the_file_goes_back_to_config_h(self):
        os.remove(self.wm.config_path)
        self.command(self.connect(), "reload")
        ipc = self.workspaces_ipc()
        self.alt("2")
        self.assertEqual(self.shown(ipc), "2")
        self.assertEqual(self.connect().request(GET_CONFIG)["config"], "")


MODES = """bindsym Mod1+r mode "resize"
bindsym x workspace outside
mode "resize" {
    bindsym x workspace inside
    bindsym Escape mode default
}
mode plain {
    bindsym y workspace plain
}
"""


class ModeTest(KeysTest):
    CONFIG = MODES

    def mode_ipc(self):
        ipc = self.connect()
        ipc.subscribe("mode", "workspace")
        return ipc

    def mode(self):
        return self.connect().request(GET_BINDING_STATE)["name"]

    def test_a_mode_has_its_own_bindings(self):
        ipc = self.mode_ipc()
        self.alt("r")
        self.assertEqual(ipc.next_event(EVENT_MODE), {"change": "resize", "pango_markup": False})
        self.assertEqual(self.mode(), "resize")
        self.press("-k", "x")
        self.assertEqual(self.shown(ipc), "inside")
        self.press("-k", "Escape")
        self.assertEqual(ipc.next_event(EVENT_MODE)["change"], "default")
        self.press("-k", "x")
        self.assertEqual(self.shown(ipc), "outside")

    def test_the_top_levels_bindings_are_off_in_a_mode(self):
        ipc = self.mode_ipc()
        self.command(self.connect(), "mode plain")
        self.press("-k", "x")  # the top level's x would show "outside"
        self.press("-k", "y")
        self.assertEqual(self.shown(ipc), "plain")

    def test_binding_modes_lists_each_mode(self):
        self.assertEqual(self.connect().request(GET_BINDING_MODES), ["default", "resize", "plain"])

    def test_the_mode_command_takes_a_quoted_name_and_refuses_an_unknown_one(self):
        self.command(self.connect(), 'mode "plain"')
        self.assertEqual(self.mode(), "plain")
        self.fails("mode nowhere")
        self.assertEqual(self.mode(), "plain")
        self.assertTrue(self.fails("mode")["parse_error"])

    def test_entering_the_mode_youre_in_says_nothing(self):
        ipc = self.mode_ipc()
        self.command(self.connect(), "mode default")
        self.command(self.connect(), "mode plain")
        self.assertEqual(ipc.next_event(EVENT_MODE)["change"], "plain")

    def test_reload_goes_back_to_the_top_level(self):
        ipc = self.mode_ipc()
        self.command(self.connect(), "mode plain")
        ipc.next_event(EVENT_MODE)
        self.command(self.connect(), "reload")
        self.assertEqual(ipc.next_event(EVENT_MODE)["change"], "default")
        self.assertEqual(self.mode(), "default")

    def test_mode_errors(self):
        for text, error in [
            ("mode a {\nbindsym x nop\n", ":1: mode a has no }"),
            ("mode a {\nmode b {\n}\n", ":2: mode a is still open: modes don't nest"),
            ("}\n", ":1: } closes no mode"),
            ("mode a\n", ":1: mode needs a name, then {"),
            ('mode "a {\n', ":1: mode needs a name, then {"),
            ("mode default {\n}\n", ":1: default is the top level, not a mode to open"),
            ('mode "a; b" {\n}\n', ":1: mode a; b: a mode's name can't hold ;"),
        ]:
            self.wm.write_config(text)
            self.assertIn(error, self.fails("reload")["error"], text)


class NoConfigModeTest(KeysTest):
    def test_config_h_keys_have_only_the_top_level(self):
        self.assertEqual(self.connect().request(GET_BINDING_MODES), ["default"])
        self.assertEqual(self.connect().request(GET_BINDING_STATE), {"name": "default"})
        self.command(self.connect(), "mode default")
        self.fails("mode resize")


class BadConfigAtStartTest(KeysTest):
    CONFIG = "bindsym Mod1+x workspace keyed\nfrobnicate\n"

    def test_tidewm_starts_with_config_h_keys_and_says_why(self):
        config = self.connect().request(GET_CONFIG)
        self.assertEqual(config["errors"], [f"{self.wm.config_path}:2: no setting is called frobnicate"])
        self.assertEqual(config["config"], "")
        self.assertIn("config.h's keys work instead", self.wm.output())
        ipc = self.workspaces_ipc()
        self.alt("2")
        self.assertEqual(self.shown(ipc), "2")


class DefaultPlaceTest(KeysTest):
    CONFIG = "bindsym Mod1+x workspace found\n"
    CONFIG_ARG = False

    def test_the_file_is_found_under_xdg_config_home(self):
        self.assertTrue(self.wm.config_path.endswith(os.path.join("xdg", "tidewm", "config")))
        self.assertEqual(self.connect().request(GET_CONFIG)["path"], self.wm.config_path)
        ipc = self.workspaces_ipc()
        self.alt("x")
        self.assertEqual(self.shown(ipc), "found")


class NoConfigTest(KeysTest):
    def test_without_a_file_config_h_keys_work(self):
        config = self.connect().request(GET_CONFIG)
        self.assertEqual((config["config"], config["errors"]), ("", []))
        self.assertEqual(self.connect().request(GET_VERSION)["loaded_config_file_name"], "")
        ipc = self.workspaces_ipc()
        self.alt("2")
        self.assertEqual(self.shown(ipc), "2")


if __name__ == "__main__":
    unittest.main()
