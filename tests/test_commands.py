"""tidewm's commands for windows and layouts: exec, focus next and prev,
floating, fullscreen, layout, nmaster, mfact, zoom and chvt."""

import os
import select
import shlex
import unittest

from harness import EVENT_WINDOW, GET_OUTPUTS, RUN_COMMAND, TIMEOUT, TidewmTest


def change_is(change, con=None):
    """Matches a window event of this change, for con if it's given."""
    return lambda e: e["change"] == change and (con is None or e["container"]["id"] == con["id"])


class CommandTest(TidewmTest):
    def fails(self, command):
        """Runs one command, asserting that it failed, and returns its result."""
        result, = self.connect().request(RUN_COMMAND, command)
        self.assertFalse(result["success"], f"{command!r}: {result}")
        return result

    def two_windows(self, ipc):
        """Opens two windows and returns their containers, the second focused
        and, as dwl puts a new window first, the master."""
        _, first = self.open_window(ipc, "test.first")
        _, second = self.open_window(ipc, "test.second")
        ipc.next_event(EVENT_WINDOW, change_is("focus", second))
        return first, second

    def rect(self, con):
        return self.windows()[con["id"]]["rect"]

    def output_rect(self):
        return self.connect().request(GET_OUTPUTS)[0]["rect"]

    def exec_output(self, payload):
        """What a payload's exec writes to {out}, a FIFO, once it runs. The
        payload's commands must all work."""
        fifo = os.path.join(self.wm.dir.name, "exec")
        os.mkfifo(fifo)
        fd = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
        self.addCleanup(os.close, fd)
        payload = payload.replace("{out}", shlex.quote(fifo))
        for result in self.connect().request(RUN_COMMAND, payload):
            self.assertTrue(result["success"], f"{payload!r}: {result}")
        self.assertTrue(select.select([fd], [], [], TIMEOUT)[0], "exec's command didn't run")
        output = b""
        while chunk := os.read(fd, 100):  # to the end, when the shell exits
            output += chunk
            select.select([fd], [], [], TIMEOUT)
        return output

    def test_exec_runs_a_shell_command(self):
        self.assertEqual(self.exec_output("exec echo ran $((1 + 1)) > {out}"), b"ran 2\n")

    def test_exec_takes_the_rest_of_the_line_semicolons_and_all(self):
        self.assertEqual(self.exec_output("exec exec > {out}; echo a; echo b"), b"a\nb\n")

    def test_execs_shell_reads_its_own_quotes(self):
        nested = """exec printf '%s ' 'a;b' "$(printf "%s" "c;d")" e\\;f > {out}"""
        self.assertEqual(self.exec_output(nested), b"a;b c;d e;f ")

    def test_an_escaped_space_at_the_end_of_exec_stays(self):
        self.assertEqual(self.exec_output("exec exec > {out}; printf '<%s>' foo\\ "), b"<foo >")

    def test_commands_before_exec_end_at_a_semicolon_quotes_and_all(self):
        output = self.exec_output("workspace Bob's tasks; exec echo ran > {out}")
        self.assertEqual(output, b"ran\n")
        self.assertIn("Bob's tasks", self.workspace_names())

    def test_empty_commands_are_skipped(self):
        self.assertEqual(self.connect().request(RUN_COMMAND, "nop;; nop;"),
                         [{"success": True}, {"success": True}])

    def test_exec_needs_a_command(self):
        self.assertTrue(self.fails("exec")["parse_error"])

    def test_focus_next_and_prev_go_round_the_windows(self):
        ipc = self.connect()
        ipc.subscribe("window")
        first, second = self.two_windows(ipc)
        self.command(self.connect(), "focus next")
        ipc.next_event(EVENT_WINDOW, change_is("focus", first))
        self.command(self.connect(), "focus next")
        ipc.next_event(EVENT_WINDOW, change_is("focus", second))
        self.command(self.connect(), "focus prev")
        ipc.next_event(EVENT_WINDOW, change_is("focus", first))

    def test_focus_next_without_a_window_fails(self):
        self.fails("focus next")

    def test_floating_toggles_and_sets(self):
        ipc = self.connect()
        ipc.subscribe("window")
        _, con = self.open_window(ipc, "test.float")
        self.command(self.connect(), "floating toggle")
        self.assertEqual(ipc.next_event(EVENT_WINDOW, change_is("floating"))["container"]["type"],
                         "floating_con")
        # Already floating: no change, so no event before the next one
        self.command(self.connect(), "floating enable")
        self.command(self.connect(), "floating disable")
        self.assertEqual(ipc.next_event(EVENT_WINDOW, change_is("floating"))["container"]["type"],
                         "con")

    def test_criteria_pick_the_window_to_float(self):
        ipc = self.connect()
        ipc.subscribe("window")
        first, _ = self.two_windows(ipc)
        self.command(self.connect(), f"[con_id={first['id']}] floating enable")
        floating = ipc.next_event(EVENT_WINDOW, change_is("floating"))
        self.assertEqual((floating["container"]["id"], floating["container"]["type"]),
                         (first["id"], "floating_con"))

    def test_floating_needs_enable_disable_or_toggle(self):
        self.assertTrue(self.fails("floating sideways")["parse_error"])

    def test_fullscreen_toggles_and_sets(self):
        ipc = self.connect()
        ipc.subscribe("window")
        self.open_window(ipc, "test.full")
        self.command(self.connect(), "fullscreen")
        self.assertEqual(ipc.next_event(EVENT_WINDOW, change_is("fullscreen_mode"))
                         ["container"]["fullscreen_mode"], 1)
        self.command(self.connect(), "fullscreen enable")
        self.command(self.connect(), "fullscreen toggle")
        self.assertEqual(ipc.next_event(EVENT_WINDOW, change_is("fullscreen_mode"))
                         ["container"]["fullscreen_mode"], 0)

    def test_a_fullscreen_window_neither_floats_nor_tiles(self):
        ipc = self.connect()
        ipc.subscribe("window")
        self.open_window(ipc, "test.full")
        self.command(self.connect(), "fullscreen enable")
        self.fails("floating toggle")

    def test_monocle_gives_each_window_the_whole_area(self):
        ipc = self.connect()
        ipc.subscribe("window")
        first, second = self.two_windows(ipc)
        self.assertNotEqual(self.rect(first), self.rect(second))
        self.command(self.connect(), "layout monocle")
        self.assertEqual(self.rect(first), self.rect(second))
        self.command(self.connect(), "layout tile")
        self.assertNotEqual(self.rect(first), self.rect(second))
        self.command(self.connect(), "layout toggle")
        self.assertEqual(self.rect(first), self.rect(second))

    def test_an_unknown_layout_fails(self):
        self.assertTrue(self.fails("layout spiral")["parse_error"])

    def test_nmaster_puts_more_windows_in_the_master_area(self):
        ipc = self.connect()
        ipc.subscribe("window")
        first, second = self.two_windows(ipc)
        self.command(self.connect(), "nmaster +1")
        self.assertEqual(self.rect(first)["x"], self.rect(second)["x"])
        self.command(self.connect(), "nmaster 1")
        self.assertNotEqual(self.rect(first)["x"], self.rect(second)["x"])
        for bad in ("1.5", "nan", "inf", "1e30"):
            self.assertTrue(self.fails(f"nmaster {bad}")["parse_error"], bad)

    def test_mfact_sizes_the_master_area(self):
        ipc = self.connect()
        ipc.subscribe("window")
        _, master = self.two_windows(ipc)
        width = self.rect(master)["width"]
        self.command(self.connect(), "mfact +0.1")
        self.assertGreater(self.rect(master)["width"], width)
        self.command(self.connect(), "mfact 0.5")
        self.assertAlmostEqual(self.rect(master)["width"], self.output_rect()["width"] / 2, delta=1)
        self.fails("mfact 0.95")
        self.fails("mfact -0.45")
        for bad in ("wide", "nan", "inf", "+nan"):
            self.assertTrue(self.fails(f"mfact {bad}")["parse_error"], bad)
        self.assertAlmostEqual(self.rect(master)["width"], self.output_rect()["width"] / 2, delta=1)

    def test_the_floating_layout_has_no_master_area(self):
        self.command(self.connect(), "layout floating")
        self.fails("mfact +0.1")

    def test_zoom_makes_the_focused_window_the_master(self):
        ipc = self.connect()
        ipc.subscribe("window")
        first, second = self.two_windows(ipc)
        left = self.output_rect()["x"]
        self.assertEqual(self.rect(second)["x"], left)
        self.command(self.connect(), "zoom")
        # The master was focused, so the next window takes its place
        self.assertEqual(self.rect(first)["x"], left)
        self.assertNotEqual(self.rect(second)["x"], left)

    def test_chvt_fails_without_a_session(self):
        self.fails("chvt 2")
        self.assertTrue(self.fails("chvt two")["parse_error"])


if __name__ == "__main__":
    unittest.main()
