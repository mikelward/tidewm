"""tidewm's layouts beside dwl's tile and monocle: three-column and two
columns + a stack, from tide's SPEC §6.1, with tide's geometry: boxes on
whole pixels that cover the window area exactly."""

import math
import unittest

from harness import EVENT_LAYOUT, GET_OUTPUTS, GET_TREE, GET_WORKSPACES, TidewmTest, power


def edge(start, length, frac):
    """Where a split at frac of [start, start + length) falls, rounded as
    tide's geometry rounds each edge."""
    return math.floor(start + length * frac + 0.5)


class LayoutTest(TidewmTest):
    def setUp(self):
        super().setUp()
        self.ipc = self.connect()
        self.ipc.subscribe("window")
        area = self.connect().request(GET_OUTPUTS)[0]["rect"]
        self.x, self.y, self.w, self.h = area["x"], area["y"], area["width"], area["height"]

    def windows_in(self, layout, n):
        """Opens n windows in that layout, in order, and returns them."""
        self.command(self.connect(), f"layout {layout}")
        return [self.open_window(self.ipc, f"test.w{i}")[1] for i in range(n)]

    def box(self, con):
        r = self.windows()[con["id"]]["rect"]
        return r["x"], r["y"], r["width"], r["height"]

    def column(self, left, right, top=0.0, bottom=1.0):
        """The box from fraction left to right of the width, top to bottom
        of the height."""
        x0, x1 = edge(self.x, self.w, left), edge(self.x, self.w, right)
        y0, y1 = edge(self.y, self.h, top), edge(self.y, self.h, bottom)
        return x0, y0, x1 - x0, y1 - y0

    def test_threecol_centers_the_master_with_a_quarter_either_side(self):
        master, right, left = self.windows_in("threecol", 3)
        self.assertEqual(self.box(master), self.column(0.25, 0.75))
        self.assertEqual(self.box(right), self.column(0.75, 1))
        self.assertEqual(self.box(left), self.column(0, 0.25))

    def test_threecol_with_two_windows_is_the_master_and_one_column(self):
        master, other = self.windows_in("threecol", 2)
        self.assertEqual(self.box(master), self.column(0, 0.5))
        self.assertEqual(self.box(other), self.column(0.5, 1))

    def test_threecol_alternates_stack_windows_right_then_left(self):
        master, *stack = self.windows_in("threecol", 5)
        self.assertEqual(self.box(master), self.column(0.25, 0.75))
        self.assertEqual([self.box(c) for c in stack], [
            self.column(0.75, 1, 0, 0.5), self.column(0, 0.25, 0, 0.5),
            self.column(0.75, 1, 0.5, 1), self.column(0, 0.25, 0.5, 1)])

    def test_a_new_window_never_moves_another_to_the_other_side(self):
        cons = self.windows_in("threecol", 3)
        for _ in range(3):
            sides = [self.box(c)[0] for c in cons]
            cons.append(self.open_window(self.ipc, "test.more")[1])
            self.assertEqual([self.box(c)[0] for c in cons[:-1]], sides)

    def test_a_window_taller_than_its_row_pushes_the_rows_below_it_down(self):
        self.command(self.connect(), "layout threecol")
        cons = [self.open_window(self.ipc, "test.master")[1]]
        # The first stack window, on the right, asks for 3/4 of the height
        cons.append(self.open_window(self.ipc, "test.tall",
                                     min_size=f"10x{self.h * 3 // 4}")[1])
        cons += [self.open_window(self.ipc, f"test.w{i}")[1] for i in range(3)]
        tall, below = self.box(cons[1]), self.box(cons[3])  # the right column
        self.assertGreater(tall[3], self.h // 2)
        self.assertEqual((below[0], below[1]), (tall[0], tall[1] + tall[3]))
        self.assertEqual(below[1] + below[3], self.y + self.h)

    def test_a_window_shorter_than_its_row_lets_the_rows_below_it_move_up(self):
        self.command(self.connect(), "layout threecol")
        cons = [self.open_window(self.ipc, "test.master")[1]]
        # The first stack window, on the right, can be only 1/8 of the height
        cons.append(self.open_window(self.ipc, "test.short",
                                     max_size=f"10000x{self.h // 8}")[1])
        cons += [self.open_window(self.ipc, f"test.w{i}")[1] for i in range(3)]
        short, below = self.box(cons[1]), self.box(cons[3])
        self.assertLess(short[3], self.h // 2)
        self.assertEqual((below[0], below[1]), (short[0], short[1] + short[3]))
        self.assertEqual(below[1] + below[3], self.y + self.h)

    def test_every_row_below_a_tall_window_moves_however_many_there_are(self):
        # twocol with 20 windows has 18 rows in its stack column. On a
        # 2000-pixel output, a window 1900 high reaches past where the 17th
        # began, and leaves room for the 17 below it.
        self.command(self.connect(), "output HEADLESS-1 mode 1280x2000")
        area = self.connect().request(GET_OUTPUTS)[0]["rect"]
        self.y, self.h = area["y"], area["height"]
        self.command(self.connect(), "layout twocol")
        cons = [self.open_window(self.ipc, f"test.m{i}")[1] for i in range(2)]
        cons.append(self.open_window(self.ipc, "test.tall", min_size="10x1900")[1])
        cons += [self.open_window(self.ipc, f"test.s{i}")[1] for i in range(17)]
        stack = [self.box(c) for c in cons[2:]]
        for upper, lower in zip(stack, stack[1:]):
            self.assertEqual(lower[1], upper[1] + upper[3])
        self.assertEqual(stack[-1][1] + stack[-1][3], self.y + self.h)

    def test_twocol_puts_two_masters_side_by_side_and_stacks_the_rest(self):
        first, second, third, fourth = self.windows_in("twocol", 4)
        self.assertEqual(self.box(first), self.column(0, 0.36))
        self.assertEqual(self.box(second), self.column(0.36, 0.72))
        self.assertEqual(self.box(third), self.column(0.72, 1, 0, 0.5))
        self.assertEqual(self.box(fourth), self.column(0.72, 1, 0.5, 1))

    def test_twocol_with_only_masters_shares_the_width(self):
        first, second = self.windows_in("twocol", 2)
        self.assertEqual((self.box(first), self.box(second)),
                         (self.column(0, 0.5), self.column(0.5, 1)))

    def test_each_layout_keeps_its_own_mfact_and_nmaster(self):
        master, _, _ = self.windows_in("tile", 3)
        self.command(self.connect(), "mfact 0.6")
        self.command(self.connect(), "layout threecol")
        self.assertEqual(self.box(master), self.column(0.25, 0.75))  # threecol's 0.5
        self.command(self.connect(), "layout twocol")
        self.assertEqual(self.box(master), self.column(0, 0.36))  # two masters at 0.72
        self.command(self.connect(), "layout tile")
        self.assertEqual(self.box(master)[2], edge(self.x, self.w, 0.6) - self.x)
        self.command(self.connect(), "layout twocol")
        self.command(self.connect(), "nmaster 1")
        self.command(self.connect(), "layout toggle")  # back to tile, then on to twocol
        self.command(self.connect(), "layout toggle")
        self.assertEqual(self.box(master), self.column(0, 0.72))


class WorkspaceLayoutTest(TidewmTest):
    """Each workspace's part on a monitor keeps its own layout, mfact and
    nmaster, as tide's SPEC §6.1 has it."""

    def layouts(self):
        return {w["name"]: w["layout"] for w in self.connect().request(GET_WORKSPACES)}

    def test_each_workspace_keeps_its_own_layout(self):
        # Empty, they'd go when not current, and take their layouts with them
        self.command(self.connect(), "persist workspace 1")
        self.command(self.connect(), "layout threecol")
        self.command(self.connect(), "persist workspace two")
        self.command(self.connect(), "workspace two")
        self.assertEqual(self.layouts(), {"1": "threecol", "two": "tile"})
        self.command(self.connect(), "layout monocle")
        self.command(self.connect(), "workspace 1")
        self.assertEqual(self.layouts(), {"1": "threecol", "two": "monocle"})
        # Its toggle is its own too: back to 1's tile, not two's
        self.command(self.connect(), "layout toggle")
        self.assertEqual(self.layouts()["1"], "tile")

    def test_mfact_is_each_workspaces_own(self):
        ipc = self.connect()
        ipc.subscribe("window")
        width = self.connect().request(GET_OUTPUTS)[0]["rect"]["width"]
        master = [self.open_window(ipc, f"test.one{i}")[1] for i in range(2)][0]
        self.command(self.connect(), "mfact 0.6")
        self.command(self.connect(), "workspace two")
        other = [self.open_window(ipc, f"test.two{i}")[1] for i in range(2)][0]
        self.assertEqual(self.windows()[other["id"]]["rect"]["width"], edge(0, width, 0.55))
        self.command(self.connect(), "workspace 1")
        self.assertEqual(self.windows()[master["id"]]["rect"]["width"], edge(0, width, 0.6))

    def test_a_workspace_made_again_starts_from_the_defaults(self):
        self.command(self.connect(), "workspace gone")
        self.command(self.connect(), "layout monocle")
        self.command(self.connect(), "workspace 1")  # gone goes: it's empty
        self.assertNotIn("gone", self.layouts())
        self.command(self.connect(), "workspace gone")
        self.assertEqual(self.layouts()["gone"], "tile")

    def test_an_ultrawide_monitors_new_parts_start_in_threecol(self):
        self.command(self.connect(), "persist workspace 1")
        self.command(self.connect(), "output HEADLESS-1 mode 3440x1440")
        self.command(self.connect(), "workspace wide")
        self.assertEqual(self.layouts(), {"1": "tile", "wide": "threecol"})

    def test_the_layout_event_says_when_a_parts_layout_changes(self):
        ipc = self.connect()
        ipc.subscribe("layout")
        self.command(self.connect(), "layout twocol")
        self.assertEqual(ipc.next_event(EVENT_LAYOUT), {
            "change": "layout", "workspace": "1", "output": "HEADLESS-1",
            "layout": "twocol", "symbol": "||="})
        self.command(self.connect(), "workspace two")
        event = ipc.next_event(EVENT_LAYOUT)
        self.assertEqual((event["workspace"], event["layout"]), ("two", "tile"))

    def test_the_layout_event_has_monocles_count_on_coming_back(self):
        ipc = self.connect()
        ipc.subscribe("window")
        self.command(self.connect(), "layout monocle")
        for i in range(2):
            self.open_window(ipc, f"test.w{i}")
        self.command(self.connect(), "workspace two")
        events = self.connect()
        events.subscribe("layout")
        self.command(self.connect(), "workspace 1")
        event = events.next_event(EVENT_LAYOUT)
        self.assertEqual((event["workspace"], event["layout"], event["symbol"]),
                         ("1", "monocle", "[2]"))

    def test_a_sleeping_outputs_layout_event_waits_until_it_wakes(self):
        # Powered off, it keeps its geometry but can't be arranged, so it
        # can't count monocle's windows yet
        self.command(self.connect(), "persist workspace 1")
        ipc = self.connect()
        ipc.subscribe("window")
        self.command(self.connect(), "layout monocle")
        for i in range(2):
            self.open_window(ipc, f"test.w{i}")
        power(self.wm, "HEADLESS-1", "off")
        self.command(self.connect(), "workspace two")
        events = self.connect()
        events.subscribe("layout")
        self.command(self.connect(), "workspace 1")
        power(self.wm, "HEADLESS-1", "on")
        event = events.next_event(EVENT_LAYOUT)
        self.assertEqual((event["workspace"], event["layout"], event["symbol"]),
                         ("1", "monocle", "[2]"))

    def test_a_layout_set_while_its_output_sleeps_is_announced_on_waking(self):
        ipc = self.connect()
        ipc.subscribe("window")
        for i in range(2):
            self.open_window(ipc, f"test.w{i}")
        power(self.wm, "HEADLESS-1", "off")
        events = self.connect()
        events.subscribe("layout")
        self.command(self.connect(), "layout monocle")
        power(self.wm, "HEADLESS-1", "on")
        event = events.next_event(EVENT_LAYOUT)
        self.assertEqual((event["layout"], event["symbol"]), ("monocle", "[2]"))

    def test_the_layout_event_counts_windows_an_output_takes_back(self):
        # Disabled, the only output leaves its windows with none; enabled
        # after a switch, it takes back its part and its windows
        self.command(self.connect(), "persist workspace 1")
        ipc = self.connect()
        ipc.subscribe("window")
        self.command(self.connect(), "layout monocle")
        for i in range(2):
            self.open_window(ipc, f"test.w{i}")
        self.command(self.connect(), "output HEADLESS-1 disable")
        self.command(self.connect(), "workspace two")
        self.command(self.connect(), "workspace 1")
        events = self.connect()
        events.subscribe("layout")
        self.command(self.connect(), "output HEADLESS-1 enable")
        event = events.next_event(EVENT_LAYOUT)
        self.assertEqual((event["workspace"], event["layout"], event["symbol"]),
                         ("1", "monocle", "[2]"))


class TwoMonitorLayoutTest(TidewmTest):
    OUTPUTS = 2

    def test_each_monitors_part_of_a_workspace_has_its_own_layout(self):
        self.command(self.connect(), "focus output HEADLESS-1")
        self.command(self.connect(), "layout threecol")
        tree = self.connect().request(GET_TREE)
        parts = {o["name"]: [w["layout"] for w in o["nodes"] if w["name"] == "1"][0]
                 for o in tree["nodes"]}
        self.assertEqual(parts, {"HEADLESS-1": "threecol", "HEADLESS-2": "tile"})

    def parts(self, workspace):
        return {o["name"]: w["layout"] for o in self.connect().request(GET_TREE)["nodes"]
                for w in o["nodes"] if w["name"] == workspace}

    def test_a_disabled_monitor_takes_its_new_parts_defaults_once_enabled(self):
        # Disabled, it has no geometry to tell it's ultrawide by
        self.command(self.connect(), "output HEADLESS-2 mode 3440x1440")
        self.command(self.connect(), "output HEADLESS-2 disable")
        self.command(self.connect(), "workspace wide")
        events = self.connect()
        events.subscribe("layout")
        self.command(self.connect(), "output HEADLESS-2 enable")
        self.assertEqual(self.parts("wide"), {"HEADLESS-1": "tile", "HEADLESS-2": "threecol"})
        event = events.next_event(EVENT_LAYOUT)
        self.assertEqual((event["workspace"], event["output"], event["layout"]),
                         ("wide", "HEADLESS-2", "threecol"))

    def test_a_disabled_monitor_gets_its_kept_part_back_once_enabled(self):
        self.command(self.connect(), "persist workspace 1")
        self.command(self.connect(), "focus output HEADLESS-2")
        self.command(self.connect(), "layout twocol")
        self.command(self.connect(), "output HEADLESS-2 disable")
        self.command(self.connect(), "workspace two")
        self.command(self.connect(), "workspace 1")
        self.command(self.connect(), "output HEADLESS-2 enable")
        self.assertEqual(self.parts("1"), {"HEADLESS-1": "tile", "HEADLESS-2": "twocol"})


if __name__ == "__main__":
    unittest.main()
