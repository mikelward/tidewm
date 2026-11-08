"""sway's focus and move left|right|up|down (docs/ipc.md), by tide's rule
for its arrow keys (its SPEC §6.6): the window that way sharing the most
height, then the nearest, then the first in stack order; past a monitor's
edge, the next monitor that way, and past the last, nothing."""

import unittest

from harness import GET_OUTPUTS, RUN_COMMAND, TidewmTest

FLOAT = 'for_window [app_id="^test.float$"] floating enable\n'


class DirectionTest(TidewmTest):
    """Two monitors side by side, tiled, as tile lays them out: the master
    on the left, the stack on the right."""
    OUTPUTS = 2
    CONFIG = FLOAT

    def setUp(self):
        super().setUp()
        self.ipc = self.connect()
        self.ipc.subscribe("window")
        self.left, self.right = sorted(self.connect().request(GET_OUTPUTS),
                                       key=lambda o: o["rect"]["x"])
        self.assertEqual(self.left["rect"]["x"] + self.left["rect"]["width"],
                         self.right["rect"]["x"])

    def open_on(self, output, app_id):
        """Opens a window on output, returning its id."""
        self.command(self.connect(), f"focus output {output['name']}")
        _, con = self.open_window(self.ipc, app_id)
        return con["id"]

    def three_on_the_left(self):
        """The master and a stack of two on the left monitor; the last is
        focused."""
        return [self.open_on(self.left, f"test.{name}") for name in ("master", "top", "bottom")]

    def focused(self):
        focused, = [i for i, w in self.windows().items() if w["focused"]]
        return focused

    def focused_output(self):
        focused, = [o["name"] for o in self.connect().request(GET_OUTPUTS) if o["focused"]]
        return focused

    def rect(self, con_id):
        return self.windows()[con_id]["rect"]

    def output_of(self, con_id):
        return self.windows()[con_id]["output"]


class FocusTest(DirectionTest):

    def test_focus_goes_to_the_window_that_way(self):
        master, top, bottom = self.three_on_the_left()
        self.command(self.connect(), "focus left")
        self.assertEqual(self.focused(), master)
        # The stack's two share as much height with the master, and are as
        # near: the first in stack order wins
        self.command(self.connect(), "focus right")
        self.assertEqual(self.focused(), top)
        self.command(self.connect(), "focus down")
        self.assertEqual(self.focused(), bottom)
        self.command(self.connect(), "focus up")
        self.assertEqual(self.focused(), top)

    def test_past_the_edge_focus_goes_to_the_next_monitors_window_at_that_edge(self):
        master, _, _ = self.three_on_the_left()
        there_master = self.open_on(self.right, "test.there.master")
        self.open_on(self.right, "test.there.stack")
        self.command(self.connect(), f"[con_id={master}] focus")
        self.command(self.connect(), "focus left")  # past the last monitor: nothing
        self.assertEqual(self.focused(), master)
        self.command(self.connect(), "focus right")
        self.command(self.connect(), "focus right")
        self.assertEqual((self.focused(), self.focused_output()),
                         (there_master, self.right["name"]))

    def test_a_monitor_with_no_window_takes_focus_itself(self):
        self.open_on(self.left, "test.only")
        self.command(self.connect(), "focus right")
        self.assertEqual(self.focused_output(), self.right["name"])
        self.assertEqual([i for i, w in self.windows().items() if w["focused"]], [])

    def test_a_fullscreen_window_on_the_next_monitor_covers_its_others(self):
        here = self.open_on(self.left, "test.here")
        self.open_on(self.right, "test.under")
        fullscreen = self.open_on(self.right, "test.fullscreen")
        self.command(self.connect(), f"[con_id={fullscreen}] fullscreen enable")
        self.command(self.connect(), f"[con_id={here}] focus")
        # The tile under it, first in stack order, ties with it
        self.command(self.connect(), "focus right")
        self.assertEqual(self.focused(), fullscreen)
        self.assertEqual(self.windows()[fullscreen]["fullscreen_mode"], 1)

    def test_a_bad_direction_is_an_error(self):
        result, = self.connect().request(RUN_COMMAND, "focus sideways")
        self.assertFalse(result["success"])
        self.assertTrue(result["parse_error"])


class MoveTest(DirectionTest):

    def test_a_tile_swaps_with_the_tile_that_way_and_keeps_focus(self):
        master, top, _ = self.three_on_the_left()
        self.command(self.connect(), f"[con_id={master}] focus")
        area = self.left["rect"]
        self.command(self.connect(), "move right")
        self.assertEqual((self.rect(top)["x"], self.focused()), (area["x"], master))
        self.assertGreater(self.rect(master)["x"], area["x"])

    def test_past_the_edge_a_tile_takes_the_place_of_the_next_monitors_at_that_edge(self):
        _, top, _ = self.three_on_the_left()
        there_master = self.open_on(self.right, "test.there.master")
        self.command(self.connect(), f"[con_id={top}] focus")
        self.command(self.connect(), "move right")
        self.assertEqual((self.output_of(top), self.focused(), self.focused_output()),
                         (self.right["name"], top, self.right["name"]))
        # The master there, at the edge it came in by
        self.assertEqual(self.rect(top)["x"], self.right["rect"]["x"])
        self.assertGreater(self.rect(there_master)["x"], self.rect(top)["x"])

    def test_past_the_last_monitor_nothing_moves(self):
        master, top, _ = self.three_on_the_left()
        before = {con_id: self.rect(con_id) for con_id in (master, top)}
        self.command(self.connect(), f"[con_id={master}] move left")
        self.command(self.connect(), f"[con_id={top}] move up")
        self.assertEqual({con_id: self.rect(con_id) for con_id in (master, top)}, before)

    def test_a_tile_on_a_hidden_workspace_doesnt_move(self):
        master, top, bottom = self.three_on_the_left()
        hidden = self.open_on(self.left, "test.hidden")
        self.command(self.connect(), f"[con_id={hidden}] move to workspace other")
        before = {con_id: self.rect(con_id) for con_id in (master, top, bottom)}
        result, = self.connect().request(RUN_COMMAND, f"[con_id={hidden}] move up")
        self.assertFalse(result["success"])
        self.assertEqual({con_id: self.rect(con_id) for con_id in (master, top, bottom)}, before)

    def test_a_floating_window_goes_to_the_next_monitor_at_the_same_spot(self):
        floating = self.open_on(self.left, "test.float")
        before = self.rect(floating)
        self.command(self.connect(), "move right")
        after = self.rect(floating)
        self.assertEqual(self.output_of(floating), self.right["name"])
        self.assertEqual((after["x"] - before["x"], after["y"], after["width"], after["height"]),
                         (self.right["rect"]["x"] - self.left["rect"]["x"], before["y"],
                          before["width"], before["height"]))
        self.assertEqual(self.focused(), floating)

    def test_a_floating_window_moves_by_a_distance_as_in_sway(self):
        floating = self.open_on(self.left, "test.float")
        before = self.rect(floating)
        self.command(self.connect(), "move left 50 px")
        self.command(self.connect(), "move down 20")
        after = self.rect(floating)
        self.assertEqual((after["x"], after["y"]), (before["x"] - 50, before["y"] + 20))
        self.assertEqual(self.output_of(floating), self.left["name"])

    def test_a_bad_distance_is_an_error(self):
        self.open_on(self.left, "test.float")
        result, = self.connect().request(RUN_COMMAND, "move left far")
        self.assertFalse(result["success"])
        self.assertTrue(result["parse_error"])


if __name__ == "__main__":
    unittest.main()
