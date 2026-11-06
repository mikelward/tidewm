"""tidewm's window rules: the config file's for_window lines, in sway's
syntax, run on each new window they match (docs/config.md)."""

import unittest

from harness import (EVENT_WINDOW, GET_CONFIG, GET_OUTPUTS, GET_WORKSPACES, RUN_COMMAND,
                     TidewmTest)

RULES = r"""for_window [app_id="^test\.float$"] floating enable
for_window [app_id=away] move to workspace far
for_window [title="^Secret"] move to workspace hidden
for_window [app_id=twice] move to workspace first
for_window [app_id=twice] move to workspace second
for_window [app_id=both shell=xdg_shell] move to workspace both; floating enable
for_window [class="^test\.x11$"] floating enable
for_window [app_id=hidden] move to workspace elsewhere; zoom
for_window [app_id="(?i)^TEST\.Caseless$"] floating enable
for_window [title="^Draft \d+ \w+?$"] move to workspace drafts
"""


class RulesTest(TidewmTest):
    CONFIG = RULES

    def new(self, ipc, app_id, title=None):
        return self.open_window(ipc, app_id, title)[1]

    def window_ipc(self):
        ipc = self.connect()
        ipc.subscribe("window")
        return ipc

    def test_a_rule_floats_the_windows_it_matches(self):
        ipc = self.window_ipc()
        self.assertEqual(self.new(ipc, "test.float")["type"], "floating_con")
        # The pattern is a regular expression, anchored here
        self.assertEqual(self.new(ipc, "test.floater")["type"], "con")

    def test_a_rule_puts_a_window_on_a_workspace_and_its_new_event_says_so(self):
        ipc = self.window_ipc()
        con = self.new(ipc, "test.away")  # app_id=away matches anywhere in it
        self.assertEqual(con["workspace"], "far")
        self.assertEqual([w["name"] for w in self.connect().request(GET_WORKSPACES)
                          if w["focused"]], ["1"])

    def test_a_rule_can_match_the_title(self):
        ipc = self.window_ipc()
        self.assertEqual(self.new(ipc, "test.secret", "Secret plans")["workspace"], "hidden")
        self.assertEqual(self.new(ipc, "test.open", "Not Secret")["workspace"], "1")

    def test_a_pattern_is_pcre2s_as_sways_are(self):
        ipc = self.window_ipc()
        # (?i) makes it ignore case
        self.assertEqual(self.new(ipc, "test.caseless")["type"], "floating_con")
        # \d, \w and a lazy count, with \w taking Unicode's letters
        self.assertEqual(self.new(ipc, "test.draft", "Draft 12 Ünïcode")["workspace"], "drafts")
        self.assertEqual(self.new(ipc, "test.nodraft", "Draft twelve")["workspace"], "1")

    def test_two_matching_rules_run_in_turn_and_leave_no_empty_workspace(self):
        ipc = self.window_ipc()
        self.assertEqual(self.new(ipc, "test.twice")["workspace"], "second")
        self.assertEqual(self.workspace_names(), ["1", "second"])

    def test_every_criterion_must_match_and_every_command_acts_on_the_new_window(self):
        ipc = self.window_ipc()
        _, focused = self.open_window(ipc, "test.focused")
        con = self.new(ipc, "test.both")
        # Moved away, the new window lost focus, but its rule's floating
        # enable still went to it, not to the window focused instead
        self.assertEqual((con["workspace"], con["type"]), ("both", "floating_con"))
        self.assertEqual(self.windows()[focused["id"]]["type"], "con")

    def test_zoom_in_a_rule_acts_on_the_new_window_not_the_focused_one(self):
        ipc = self.window_ipc()
        _, first = self.open_window(ipc, "test.first")
        _, second = self.open_window(ipc, "test.second")
        left = self.connect().request(GET_OUTPUTS)[0]["rect"]["x"]
        self.assertEqual(self.windows()[first["id"]]["rect"]["x"], left)
        # The new window is moved away, so zooming it does nothing; zooming
        # the focused window would make test.second the master
        self.new(ipc, "test.hidden")
        self.assertEqual(self.windows()[first["id"]]["rect"]["x"], left)

    def test_an_x11_window_matches_by_class(self):
        self.need(self.wm.x11, "tidewm wasn't built with XWayland, or has no Xwayland")
        ipc = self.window_ipc()
        self.wm.open_x11_window("test.x11", "Named")
        new = ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new")
        self.assertEqual(new["container"]["type"], "floating_con")

    def test_reload_brings_new_rules_for_later_windows(self):
        self.wm.write_config('for_window [app_id="^test\\.later$"] move to workspace later\n')
        self.command(self.connect(), "reload")
        ipc = self.window_ipc()
        self.assertEqual(self.new(ipc, "test.later")["workspace"], "later")
        self.assertEqual(self.new(ipc, "test.float")["type"], "con")

    def test_rule_errors(self):
        for text, error in [
            ("for_window floating enable\n", ":1: for_window needs [criteria], then commands"),
            ("for_window [app_id=x]\n", ":1: for_window needs [criteria], then commands"),
            ("for_window [] nop\n", ":1: for_window needs [criteria], then commands"),
            ("for_window [colour=red] nop\n", ":1: for_window: no criterion is called colour"),
            ("for_window [app_id=a app_id=b] nop\n", ":1: for_window: app_id is there twice"),
            ('for_window [title="open] nop\n', ":1: for_window: title's value has no closing \""),
            ("for_window [title=(] nop\n", ":1: for_window: title=(: "),
            ("for_window [app_id=x\n", ":1: for_window's criteria have no ]"),
            ("for_window [app_id=x nop\n", ":1: for_window's criteria are key=value, in [ ]"),
            ("for_window [app_id] nop\n", ":1: for_window's criteria are key=value, in [ ]"),
            ("mode m {\nfor_window [app_id=x] nop\n}\n", ":2: for_window can't be in a mode"),
        ]:
            self.wm.write_config(text)
            result, = self.connect().request(RUN_COMMAND, "reload")
            self.assertFalse(result["success"], text)
            self.assertIn(error, result["error"], text)
        self.assertEqual(self.connect().request(GET_CONFIG)["config"], RULES)


class RuleMatchErrorTest(TidewmTest):
    """A match PCRE2 can't finish, here one past the limit the pattern
    sets itself, runs nothing and says why."""
    CONFIG = 'for_window [app_id="(*LIMIT_MATCH=1)^(test)+$"] floating enable\n'

    def test_a_match_that_errs_runs_nothing_and_is_logged(self):
        ipc = self.connect()
        ipc.subscribe("window")
        _, con = self.open_window(ipc, "test")
        self.assertEqual(con["type"], "con")
        self.assertIn("tidewm: for_window rule 1: couldn't match its app_id: match limit exceeded",
                      self.wm.output())


class ConfigHRulesTest(TidewmTest):
    """config.h's example rule puts app ID firefox_EXAMPLE on workspace 9."""

    def test_without_a_file_config_h_rules_apply(self):
        ipc = self.connect()
        ipc.subscribe("window")
        self.assertEqual(self.open_window(ipc, "firefox_EXAMPLE")[1]["workspace"], "9")


class FileReplacesConfigHRulesTest(TidewmTest):
    CONFIG = "for_window [app_id=nothing] nop\n"

    def test_a_file_s_rules_replace_config_h_s(self):
        ipc = self.connect()
        ipc.subscribe("window")
        self.assertEqual(self.open_window(ipc, "firefox_EXAMPLE")[1]["workspace"], "1")


if __name__ == "__main__":
    unittest.main()
