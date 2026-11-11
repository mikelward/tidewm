"""tidewm's config file, in sway's syntax: bindsym and set, read at startup
and again by reload (docs/config.md)."""

import os
import select
import shlex
import subprocess
import unittest

from harness import (EVENT_BINDING, EVENT_MODE, EVENT_WORKSPACE, GET_BINDING_MODES,
                     GET_BINDING_STATE, GET_BINDINGS, GET_CONFIG, GET_NAMED_BINDINGS,
                     GET_VERSION, ROOT, RUN_COMMAND, TIMEOUT, TidewmTest)

POINTER = os.path.join(ROOT, "tests", "pointer")

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
        self.assertEqual(config, {"config": CONFIG, "included_configs": [], "path": self.wm.config_path,
                                  "errors": []})
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

    def test_an_unknown_flag_is_an_error(self):
        self.wm.write_config("bindsym --border Mod1+x nop\n")
        self.assertIn(":1: bindsym has no flag --border", self.fails("reload")["error"])

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


class BindingsTest(TidewmTest):
    """GET_BINDINGS, tidewm's own: the config file's bindings, for a shell's
    list of keyboard shortcuts."""
    CONFIG = """set $mod Mod4
bindsym $mod+Return exec foot
bindsym --release $mod+d exec launcher
bindsym --kept-open Ctrl+w kill
mode resize {
    bindsym --locked Escape mode default
}
bindsym $mod+r mode resize
bindswitch --locked lid:on output eDP-1 disable
bindswitch tablet:toggle workspace tablet
"""

    def test_each_binding_with_its_keys_command_and_mode(self):
        def key(keys, command, mode="default", release=False, locked=False, kept_open=False):
            return {"type": "key", "keys": keys, "command": command, "mode": mode,
                    "release": release, "locked": locked, "kept_open": kept_open, "name": ""}

        def switch(keys, command, locked=False):
            return {"type": "switch", "keys": keys, "command": command, "mode": "default",
                    "release": False, "locked": locked, "kept_open": False, "name": ""}

        self.assertEqual(self.connect().request(GET_BINDINGS), [
            key("Mod4+Return", "exec foot"),
            key("Mod4+d", "exec launcher", release=True),
            key("Ctrl+w", "kill", kept_open=True),
            key("Mod4+r", "mode resize"),
            key("Escape", "mode default", mode="resize", locked=True),
            switch("lid:on", "output eDP-1 disable", locked=True),
            switch("tablet:toggle", "workspace tablet"),
        ])

    def test_a_reload_lists_the_new_ones(self):
        self.wm.write_config("bindsym Mod1+x workspace x\n")
        self.command(self.connect(), "reload")
        self.assertEqual([b["keys"] for b in self.connect().request(GET_BINDINGS)], ["Mod1+x"])


class NamedBindingsTest(KeysTest):
    """bindsym --name and rebind, tidewm's own: a shell's keyboard
    shortcuts give a named binding other keys in a file the config
    includes."""
    CONFIG = """bindsym --name go_a Mod1+a workspace a
bindsym --name go_b Mod1+b workspace b
bindsym --name go_b Mod1+c workspace b
bindsym --name=pinned --locked Mod1+p workspace p
bindsym Mod1+z workspace marker
mode resize {
    bindsym --name grow Mod1+g workspace grown
}
include tide-keys.conf
"""

    def rebind(self, text):
        """Writes the shell's tide-keys.conf and reloads."""
        with open(os.path.join(os.path.dirname(self.wm.config_path), "tide-keys.conf"), "w") as f:
            f.write(text)
        self.command(self.connect(), "reload")

    def ran(self, key):
        """The workspace Alt and key shows, or "marker" if it runs nothing:
        Alt+z, pressed after it, shows that. Starts on another workspace,
        so either is a change."""
        self.command(self.connect(), "workspace start")
        ipc = self.workspaces_ipc()
        self.alt(key)
        self.alt("z")
        return self.shown(ipc)

    def named(self):
        return {b["name"]: (b["keys"], b["default_keys"])
                for b in self.connect().request(GET_NAMED_BINDINGS)}

    def test_get_named_bindings_lists_each_name_with_its_keys(self):
        self.assertEqual(self.connect().request(GET_NAMED_BINDINGS), [
            {"name": "go_a", "type": "key", "keys": ["Mod1+a"], "default_keys": ["Mod1+a"],
             "command": "workspace a", "mode": "default", "release": False, "locked": False,
             "kept_open": False},
            {"name": "go_b", "type": "key", "keys": ["Mod1+b", "Mod1+c"],
             "default_keys": ["Mod1+b", "Mod1+c"], "command": "workspace b", "mode": "default",
             "release": False, "locked": False, "kept_open": False},
            {"name": "pinned", "type": "key", "keys": ["Mod1+p"], "default_keys": ["Mod1+p"],
             "command": "workspace p", "mode": "default", "release": False, "locked": True,
             "kept_open": False},
            {"name": "grow", "type": "key", "keys": ["Mod1+g"], "default_keys": ["Mod1+g"],
             "command": "workspace grown", "mode": "resize", "release": False, "locked": False,
             "kept_open": False},
        ])
        self.assertEqual({b["keys"]: b["name"] for b in self.connect().request(GET_BINDINGS)}, {
            "Mod1+a": "go_a", "Mod1+b": "go_b", "Mod1+c": "go_b", "Mod1+p": "pinned",
            "Mod1+z": "", "Mod1+g": "grow"})

    def test_rebind_replaces_a_bindings_keys(self):
        self.rebind("rebind go_b Mod1+x\n")
        self.assertEqual(self.ran("x"), "b")
        self.assertEqual(self.ran("b"), "marker", "go_b's own keys are gone")
        self.assertEqual(self.ran("c"), "marker")
        self.assertEqual(self.named()["go_b"], (["Mod1+x"], ["Mod1+b", "Mod1+c"]))

    def test_rebind_keeps_the_bindings_flags(self):
        self.rebind("rebind pinned Mod1+q\n")
        self.assertEqual(next(b for b in self.connect().request(GET_BINDINGS)
                              if b["name"] == "pinned")["locked"], True)

    def test_rebind_none_turns_a_binding_off(self):
        self.rebind("rebind go_a none\n")
        self.assertEqual(self.ran("a"), "marker")
        self.assertEqual(self.named()["go_a"], ([], ["Mod1+a"]))

    def test_a_rebinds_keys_are_taken_from_the_binding_that_had_them(self):
        self.rebind("rebind go_a Mod1+b\n")
        self.assertEqual(self.ran("b"), "a")
        self.assertEqual(self.ran("c"), "b", "go_b keeps its other keys")
        self.assertEqual(self.named()["go_b"], (["Mod1+c"], ["Mod1+b", "Mod1+c"]))

    def test_a_mode_binding_is_rebound_in_its_mode(self):
        self.rebind("rebind grow Mod1+h\n")
        self.command(self.connect(), "mode resize")
        self.assertEqual(self.ran("h"), "grown")

    def test_an_unknown_name_is_logged_and_skipped(self):
        self.rebind("rebind gone Mod1+x\nrebind go_a Mod1+y\n")
        self.assertIn("rebind gone: no binding is called gone, so it's skipped", self.wm.output())
        self.assertEqual(self.ran("y"), "a", "the rest of the file loads")

    def test_named_binding_errors(self):
        for text, error in [
            ("bindsym --name Go Mod1+x nop\n", ":1: bindsym --name Go: a name is a lower-case letter"),
            ("bindsym --name Mod1+x nop\n", ":1: bindsym needs keys and a command"),
            ("bindsym --name\n", ":1: bindsym --name needs a name"),
            ("bindsym --name x Mod1+x nop\nrebind x Mod1+NoSuchKey\n",
             ":2: rebind x Mod1+NoSuchKey: no modifier or key is called NoSuchKey"),
            ("bindsym --name x Mod1+x nop\nrebind x none Mod1+y\n", ":2: rebind x: none is alone"),
            ("rebind x\n", ":1: rebind needs a binding's name, then its keys, or none"),
            ("bindsym --name x Mod1+x nop\nmode m {\nrebind x Mod1+y\n}\n",
             ":3: rebind can't be in a mode"),
        ]:
            self.wm.write_config(text)
            self.assertIn(error, self.fails("reload")["error"], text)

    def test_a_later_line_with_another_command_redefines_the_name(self):
        """A config that includes tide's and binds tide's names to its own
        commands, as conf's does: its lines win, and the name's earlier
        keys go with what they ran."""
        self.rebind("bindsym --name go_b Mod1+d workspace d\n"
                    "bindsym --name go_b Mod1+e workspace d\n")
        self.assertEqual(self.ran("d"), "d")
        self.assertEqual(self.ran("e"), "d", "a second line for the new command adds a key")
        self.assertEqual(self.ran("b"), "marker", "the old command's keys are gone")
        self.assertEqual(self.ran("c"), "marker")
        self.assertEqual(self.named()["go_b"], (["Mod1+d", "Mod1+e"], ["Mod1+d", "Mod1+e"]))

    def test_a_later_line_with_other_flags_or_mode_redefines_the_name(self):
        self.rebind("bindsym --name go_a --locked Mod1+a workspace a\n"
                    "mode resize {\n    bindsym --name go_b Mod1+b workspace b\n}\n")
        bindings = {b["name"]: b for b in self.connect().request(GET_NAMED_BINDINGS)}
        self.assertEqual(bindings["go_a"]["locked"], True)
        self.assertEqual(bindings["go_b"]["mode"], "resize")
        self.assertEqual(bindings["go_b"]["keys"], ["Mod1+b"])
        self.assertEqual(self.ran("c"), "marker", "go_b's default-mode keys are gone")

    def test_a_bad_rebind_keeps_the_keys_there_were(self):
        self.rebind("rebind go_a Mod1+x\n")
        with open(os.path.join(os.path.dirname(self.wm.config_path), "tide-keys.conf"), "w") as f:
            f.write("rebind go_a Mod1+NoSuchKey\n")
        self.fails("reload")
        self.assertEqual(self.ran("x"), "a")


class NoConfigBindingsTest(TidewmTest):
    def test_without_a_file_there_are_none(self):
        self.assertEqual(self.connect().request(GET_BINDINGS), [])


class IncludeTest(KeysTest):
    """include lines: each file a pattern matches is read as if its lines
    were there, as sway's include does, so a shell can write a file of its
    own that the config takes in."""

    CONFIG = "bindsym Mod1+x workspace main\n"

    def write(self, name, text):
        path = os.path.join(os.path.dirname(self.wm.config_path), name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(text)
        return path

    def pressed(self, key="z"):
        ipc = self.workspaces_ipc()
        self.alt(key)
        return self.shown(ipc)

    def test_an_included_files_lines_count(self):
        path = self.write("extra.conf", "bindsym Mod1+z workspace included\n")
        self.wm.write_config("bindsym Mod1+x workspace main\ninclude extra.conf\n")
        self.command(self.connect(), "reload")
        self.assertEqual(self.pressed("z"), "included")
        self.assertEqual(self.pressed("x"), "main")
        config = self.connect().request(GET_CONFIG)
        self.assertEqual(config["included_configs"],
                         [{"path": path, "raw_config": "bindsym Mod1+z workspace included\n"}])

    def test_variables_carry_into_and_out_of_an_included_file(self):
        self.write("extra.conf", "bindsym Mod1+z workspace $in\nset $out from_include\n")
        self.wm.write_config("set $in from_main\ninclude extra.conf\nbindsym Mod1+x workspace $out\n")
        self.command(self.connect(), "reload")
        self.assertEqual(self.pressed("z"), "from_main")
        self.assertEqual(self.pressed("x"), "from_include")

    def test_a_pattern_includes_each_file_it_matches_in_order(self):
        self.write("tide-a.conf", "bindsym Mod1+z workspace first\n")
        self.write("tide-b.conf", "bindsym Mod1+z workspace second\n")
        self.wm.write_config(f"include {os.path.dirname(self.wm.config_path)}/tide-*.conf\n")
        self.command(self.connect(), "reload")
        self.assertEqual(self.pressed("z"), "second", "the later file's binding wins")

    def test_a_relative_include_is_from_its_files_directory_taken_as_it_is(self):
        # A directory whose name is a pattern too: sub[x] would match subx
        self.write("sub[x]/outer.conf", "include inner.conf\n")
        self.write("sub[x]/inner.conf", "bindsym Mod1+z workspace inner\n")
        self.write("subx/inner.conf", "bindsym Mod1+z workspace wrong\n")
        self.wm.write_config("include sub*/outer.conf\n")
        self.command(self.connect(), "reload")
        self.assertEqual(self.pressed("z"), "inner")

    def test_a_missing_file_or_a_pattern_nothing_matches_includes_nothing(self):
        self.wm.write_config("bindsym Mod1+x workspace main\ninclude nosuch.conf\n"
                             "include \"not yet/tide-*.conf\"\n")
        self.command(self.connect(), "reload")
        self.assertEqual(self.pressed("x"), "main")
        self.assertEqual(self.connect().request(GET_CONFIG)["included_configs"], [])

    def test_reload_reads_an_included_file_again(self):
        self.write("extra.conf", "bindsym Mod1+z workspace before\n")
        self.wm.write_config("include extra.conf\n")
        self.command(self.connect(), "reload")
        self.write("extra.conf", "bindsym Mod1+z workspace after\n")
        self.command(self.connect(), "reload")
        self.assertEqual(self.pressed("z"), "after")

    def test_an_included_files_errors_name_it_and_the_old_config_stays(self):
        path = self.write("extra.conf", "bindsym Mod1+z nop\nfrobnicate now\n")
        self.wm.write_config("include extra.conf\n")
        self.assertEqual(self.fails("reload")["error"], f"{path}:2: no setting is called frobnicate")
        self.assertEqual(self.pressed("x"), "main")

    def test_an_included_keymap_error_names_its_file(self):
        path = self.write("keys.conf", "input type:keyboard xkb_layout nosuchlayout\n")
        self.wm.write_config("include keys.conf\n")
        self.assertIn(f"{path}:1: input type:keyboard: XKB can't make a keymap", self.fails("reload")["error"])

    def test_a_keymap_error_names_its_xkb_lines_file_not_a_later_lines(self):
        path = self.write("keys.conf", "input type:keyboard xkb_layout nosuchlayout\n")
        self.wm.write_config("include keys.conf\ninput type:keyboard repeat_rate 30\n")
        self.assertIn(f"{path}:1: input type:keyboard: XKB can't make a keymap", self.fails("reload")["error"])

    def test_a_file_that_includes_itself_is_an_error(self):
        self.write("loop.conf", "include loop.conf\n")
        self.wm.write_config("include loop.conf\n")
        self.assertIn("includes nest over 10 deep", self.fails("reload")["error"])

    def test_include_needs_a_file_and_is_only_at_the_top_level(self):
        self.wm.write_config("include\n")
        self.assertIn(":1: include needs a file", self.fails("reload")["error"])
        self.wm.write_config("mode m {\n    include extra.conf\n}\n")
        self.assertIn(":2: include can't be in a mode", self.fails("reload")["error"])
        self.write("block.conf", "mode m {\n")
        self.wm.write_config("include block.conf\n}\n")
        error = self.fails("reload")["error"]
        self.assertIn("block.conf:1: mode m has no }", error, "a block opened in a file closes in it")


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


class ReleaseTest(KeysTest):
    CONFIG = """bindsym --release Super_L workspace tapped
bindsym Mod1+z workspace marker
bindsym Mod1+r workspace pressed
bindsym --release Mod1+r workspace released
"""

    def test_a_release_binding_runs_when_its_key_is_tapped(self):
        ipc = self.workspaces_ipc()
        self.press("-k", "Super_L")
        self.assertEqual(self.shown(ipc), "tapped")

    def test_another_key_pressed_meanwhile_is_no_tap(self):
        ipc = self.workspaces_ipc()
        self.press("-P", "Super_L", "-k", "x", "-p", "Super_L")
        self.alt("z")
        self.assertEqual(self.shown(ipc), "marker")

    def test_press_and_release_bindings_of_the_same_keys_run_in_turn(self):
        ipc = self.workspaces_ipc()
        self.alt("r")
        self.assertEqual([self.shown(ipc), self.shown(ipc)], ["pressed", "released"])


class TapTest(KeysTest):
    """Super tapped alone, tide's launcher key (its SPEC §8.2), and not
    Super+click, Super+drag or Super with a scroll."""
    CONFIG = """bindsym --release Super_L workspace tapped
bindsym Super_L workspace down
bindsym Mod1+z workspace marker
"""

    def pointer_with_super(self, *actions):
        """Holds Super down, as wtype does while it waits on its input, does
        the pointer's actions, and lets Super go."""
        ipc = self.workspaces_ipc()
        typist = subprocess.Popen(["wtype", "-P", "Super_L", "-", "-p", "Super_L"],
                                  env=self.wm.env, stdin=subprocess.PIPE)
        self.wm.clients.append(typist)
        self.assertEqual(self.shown(ipc), "down")
        if actions:
            subprocess.run([POINTER, *actions], env=self.wm.env, check=True, timeout=TIMEOUT)
        typist.stdin.close()
        typist.wait(TIMEOUT)
        # Had the release been a tap, its workspace would come first
        self.alt("z")
        return self.shown(ipc)

    def test_a_tap_alone_runs(self):
        self.assertEqual(self.pointer_with_super(), "tapped")

    def test_a_click_meanwhile_is_no_tap(self):
        self.assertEqual(self.pointer_with_super("click"), "marker")

    def test_a_drag_meanwhile_is_no_tap(self):
        self.assertEqual(self.pointer_with_super("press", "left", "move", "100", "100",
                                                 "1280", "720", "release", "left"), "marker")

    def test_a_scroll_meanwhile_is_no_tap(self):
        self.assertEqual(self.pointer_with_super("scroll"), "marker")


class BindSwitchTest(KeysTest):
    """sway's bindswitch, for the lid, with the switch command moving
    tidewm's own switch device, as the headless backend has none."""
    CONFIG = """bindswitch lid:on output HEADLESS-1 disable
bindswitch --locked lid:off output HEADLESS-1 enable
bindswitch tablet:toggle nop tablet
"""

    def test_switch_bindings_load(self):
        config = self.connect().request(GET_CONFIG)
        self.assertEqual((config["config"], config["errors"]), (self.CONFIG, []))

    def test_bad_switch_bindings_are_errors(self):
        for text, error in [
            ("bindswitch lid:open nop\n", ":1: bindswitch lid:open: a switch is lid or tablet, "
             "and its state on, off or toggle"),
            ("bindswitch hinge:on nop\n", ":1: bindswitch hinge:on: a switch is lid or tablet, "
             "and its state on, off or toggle"),
            ("bindswitch lid nop\n", ":1: bindswitch lid: a switch is lid or tablet, "
             "and its state on, off or toggle"),
            ("bindswitch lid:on\n", ":1: bindswitch needs a switch and a command"),
            ("bindswitch --reload lid:on nop\n", ":1: bindswitch has no flag --reload"),
            ("mode m {\nbindswitch lid:on nop\n}\n", ":2: bindswitch can't be in a mode"),
        ]:
            self.wm.write_config(text)
            self.assertIn(error, self.fails("reload")["error"], text)
        self.assertEqual(self.connect().request(GET_CONFIG)["config"], self.CONFIG)


class SwitchTest(KeysTest):
    CONFIG = """bindswitch lid:on workspace closed
bindswitch lid:off workspace opened
bindswitch tablet:toggle workspace tablet
"""

    def test_a_switch_runs_the_binding_for_its_state(self):
        ipc = self.workspaces_ipc()
        self.command(self.connect(), "switch lid on")
        self.assertEqual(self.shown(ipc), "closed")
        self.command(self.connect(), "switch lid off")
        self.assertEqual(self.shown(ipc), "opened")

    def test_toggle_runs_for_either_state(self):
        ipc = self.workspaces_ipc()
        self.command(self.connect(), "switch tablet on")
        self.assertEqual(self.shown(ipc), "tablet")
        self.command(self.connect(), "workspace 1")
        self.assertEqual(self.shown(ipc), "1")
        self.command(self.connect(), "switch tablet off")
        self.assertEqual(self.shown(ipc), "tablet")

    def test_a_switch_binding_that_runs_says_so(self):
        ipc = self.connect()
        ipc.subscribe("binding")
        self.command(self.connect(), "switch lid on")
        self.assertEqual(ipc.next_event(EVENT_BINDING), {"change": "run", "binding": {
            "command": "workspace closed", "event_state_mask": [], "input_code": 0,
            "symbol": None, "input_type": "switch"}})

    def test_the_switch_command_takes_a_switch_and_a_state(self):
        for command in ["switch", "switch lid", "switch lid open", "switch hinge on",
                        "switch lid on now"]:
            self.fails(command)


class BindingEventTest(KeysTest):
    CONFIG = """bindsym --release Super_L nop launcher
bindsym Mod1+Shift+z nop marker
"""

    def test_a_binding_that_runs_says_so(self):
        ipc = self.connect()
        ipc.subscribe("binding")
        self.press("-k", "Super_L")
        self.assertEqual(ipc.next_event(EVENT_BINDING), {"change": "run", "binding": {
            "command": "nop launcher", "event_state_mask": [], "input_code": 0,
            "symbol": "Super_L", "input_type": "keyboard"}})
        self.alt("z", shift=True)
        binding = ipc.next_event(EVENT_BINDING)["binding"]
        self.assertEqual((binding["command"], binding["event_state_mask"], binding["symbol"]),
                         ("nop marker", ["Shift", "Mod1"], "z"))


class LockedSwitchTest(KeysTest):
    CONFIG = """bindswitch lid:on workspace closed
bindswitch --locked lid:off workspace opened
"""

    def test_only_locked_switch_bindings_run_on_the_lock_screen(self):
        ipc = self.workspaces_ipc()
        self.lock()
        self.command(self.connect(), "switch lid on")  # not --locked: nothing
        self.command(self.connect(), "switch lid off")
        self.assertEqual(self.shown(ipc), "opened")


class LockedTest(KeysTest):
    CONFIG = "bindsym --locked Mod1+x workspace locked\nbindsym Mod1+y workspace plain\n"

    def test_only_locked_bindings_run_on_the_lock_screen(self):
        ipc = self.workspaces_ipc()
        locker = self.lock()
        self.alt("y")  # not --locked: nothing yet
        self.alt("x")
        self.assertEqual(self.shown(ipc), "locked")
        self.unlock(locker)
        self.alt("y")
        self.assertEqual(self.shown(ipc), "plain")


class NoConfigLockTest(KeysTest):
    def test_config_h_keys_are_off_on_the_lock_screen(self):
        ipc = self.workspaces_ipc()
        locker = self.lock()
        self.alt("2")
        self.unlock(locker)
        self.alt("3")
        self.assertEqual(self.shown(ipc), "3")


class BadConfigAtStartTest(KeysTest):
    CONFIG = "bindsym Mod1+x workspace keyed\nfrobnicate\nbindsym Mod1+y workspace later\n"

    def test_tidewm_starts_without_the_bad_line_and_says_why(self):
        config = self.connect().request(GET_CONFIG)
        self.assertEqual(config["errors"], [f"{self.wm.config_path}:2: no setting is called frobnicate"])
        self.assertEqual(config["config"], self.CONFIG)
        self.assertIn("left out the lines above", self.wm.output())
        ipc = self.workspaces_ipc()
        self.alt("x")
        self.assertEqual(self.shown(ipc), "keyed", "a line before the bad one works")
        self.alt("y")
        self.assertEqual(self.shown(ipc), "later", "and one after it")

    def test_a_reload_with_an_error_still_keeps_what_was_loaded(self):
        self.wm.write_config("bindsym Mod1+x workspace other\nfrobnicate\n")
        self.assertIn(":2: no setting is called frobnicate", self.fails("reload")["error"])
        ipc = self.workspaces_ipc()
        self.alt("x")
        self.assertEqual(self.shown(ipc), "keyed")


class NoKeysAtStartTest(KeysTest):
    # Every binding failed, and the rest of the file loaded
    CONFIG = "bindsym --frobnicate Mod1+x workspace keyed\ndim_inactive 0.5\n"

    def test_a_file_that_binds_no_key_keeps_config_hs_keys(self):
        config = self.connect().request(GET_CONFIG)
        self.assertEqual(config["config"], self.CONFIG)
        self.assertIn("binds no key, so config.h's keys work with it", self.wm.output())
        ipc = self.workspaces_ipc()
        self.alt("2")
        self.assertEqual(self.shown(ipc), "2")

    def test_a_reload_that_binds_a_key_takes_config_hs_away(self):
        self.wm.write_config("bindsym Mod1+x workspace keyed\n")
        result, = self.connect().request(RUN_COMMAND, "reload")
        self.assertTrue(result["success"], result)
        ipc = self.workspaces_ipc()
        self.alt("2")
        self.alt("x")
        self.assertEqual(self.shown(ipc), "keyed", "Alt+2 is no longer a key")


class KeysOnlyInAModeTest(KeysTest):
    CONFIG = 'mode "inner" {\n    bindsym Mod1+x workspace keyed\n}\n'

    def test_a_mode_of_the_files_has_its_own_keys(self):
        ipc = self.workspaces_ipc()
        self.alt("2")
        self.assertEqual(self.shown(ipc), "2", "config.h's keys at the top level")
        result, = self.connect().request(RUN_COMMAND, "mode inner")
        self.assertTrue(result["success"], result)
        self.alt("x")
        self.assertEqual(self.shown(ipc), "keyed")


class BadModeAtStartTest(KeysTest):
    # sway's flag, which tidewm's mode doesn't take
    CONFIG = ('bindsym Mod1+x workspace keyed\n'
              'mode --pango_markup "resize" {\n'
              '    bindsym Mod1+x workspace leaked\n'
              '    bindsym Mod1+y workspace leaked\n'
              '} # resize\n'
              'bindsym Mod1+z workspace after\n')

    def test_a_mode_that_fails_to_open_takes_its_lines_with_it(self):
        errors = self.connect().request(GET_CONFIG)["errors"]
        self.assertEqual(errors, [f"{self.wm.config_path}:2: mode needs a name, then {{"])
        ipc = self.workspaces_ipc()
        self.alt("y")
        self.alt("x")
        self.assertEqual(self.shown(ipc), "keyed", "the mode's bindings aren't the top level's")
        self.alt("z")
        self.assertEqual(self.shown(ipc), "after", "and the lines after its } are read")


class BadNamedLineAtStartTest(KeysTest):
    CONFIG = ("bindsym --name go Mod1+x workspace keyed\n"
              "bindsym --name go Mod1+NoSuchKey workspace other\n")

    def test_a_named_line_with_bad_keys_leaves_the_name_as_it_was(self):
        self.assertIn(":2: bindsym Mod1+NoSuchKey: no modifier or key is called NoSuchKey",
                      self.connect().request(GET_CONFIG)["errors"][0])
        ipc = self.workspaces_ipc()
        self.alt("x")
        self.assertEqual(self.shown(ipc), "keyed")


class BadKeymapAtStartTest(KeysTest):
    CONFIG = "bindsym Mod1+x workspace keyed\ninput type:keyboard xkb_layout nosuchlayout\n"

    def test_a_keymap_xkb_cant_make_leaves_only_the_layout_out(self):
        config = self.connect().request(GET_CONFIG)
        self.assertEqual(config["config"], self.CONFIG)
        self.assertIn(":2: input type:keyboard: XKB can't make a keymap", config["errors"][0])
        self.assertIn("left out the lines above", self.wm.output())
        ipc = self.workspaces_ipc()
        # The keyboard still types, in the layout it had, and the file's keys work
        self.alt("x")
        self.assertEqual(self.shown(ipc), "keyed")


class BadEnvironmentKeymapTest(KeysTest):
    ENV = {"XKB_DEFAULT_LAYOUT": "nosuchlayout"}

    def test_tidewm_starts_with_xkbs_own_keymap_and_says_why(self):
        self.assertIn("tidewm: XKB can't make a keymap of the environment's "
                      "XKB_DEFAULT_LAYOUT=nosuchlayout;", self.wm.output())
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
