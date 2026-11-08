"""Dragging the master/stack edge, as tide's SPEC §6.6 has it: a plain
left press within resize_grab pixels either side of the edge between the
masters and the stack drags it, setting mfact, and a lone window's sides
drag its width, centered. Anything over the edge takes the press as usual,
as do an unfocused monitor and a fullscreen or maximized window."""

import os
import select
import subprocess
import unittest

from harness import EVENT_WINDOW, GET_OUTPUTS, ROOT, RUN_COMMAND, TIMEOUT, TidewmTest
from test_mouse import HOLD, POINTER, MouseTest

LAUNCHER = os.path.join(ROOT, "tests", "launcher")

FLOAT = 'for_window [app_id="^test.float$"] floating enable\n'


class SplitTest(TidewmTest):
    """Helpers, with no tests of their own."""

    def setUp(self):
        super().setUp()
        self.ipc = self.connect()
        self.ipc.subscribe("window")

    def size(self):
        outputs = self.connect().request(GET_OUTPUTS)
        return [str(max(o["rect"]["x"] + o["rect"]["width"] for o in outputs)),
                str(max(o["rect"]["y"] + o["rect"]["height"] for o in outputs))]

    def area(self, name="HEADLESS-1"):
        r, = [o["rect"] for o in self.connect().request(GET_OUTPUTS) if o["name"] == name]
        return r["x"], r["y"], r["width"], r["height"]

    def pointer(self, *actions):
        """Does the pointer's actions, which tidewm has all had once it
        returns."""
        subprocess.run([POINTER, *actions], env=self.wm.env, check=True, timeout=TIMEOUT)

    def drag(self, x, y, to_x, to_y=None, button="left"):
        """Presses button at x, y, moves to to_x, to_y, and lets go."""
        size = self.size()
        self.pointer("move", str(x), str(y), *size, "press", button,
                     "move", str(to_x), str(y if to_y is None else to_y), *size,
                     "release", button)

    def windows_in(self, layout, n, mfact=None):
        """Opens n windows in that layout, in order, and returns their ids."""
        self.command(self.connect(), f"layout {layout}")
        if mfact:
            self.command(self.connect(), f"mfact {mfact}")
        return [self.open_window(self.ipc, f"test.w{i}")[1]["id"] for i in range(n)]

    def box(self, con_id):
        r = self.windows()[con_id]["rect"]
        return r["x"], r["y"], r["width"], r["height"]

    def boxes(self, ids):
        return [self.box(i) for i in ids]

    def line(self, process):
        """process's next line. It reads the pipe itself, keeping what's
        past a line for the next call, since a buffered readline would
        hide lines already read from select."""
        buffered = getattr(process, "split_lines", b"")
        while b"\n" not in buffered:
            self.assertTrue(select.select([process.stdout], [], [], TIMEOUT)[0],
                            "it said nothing")
            chunk = os.read(process.stdout.fileno(), 4096)
            self.assertTrue(chunk, "it went")
            buffered += chunk
        line, process.split_lines = buffered.split(b"\n", 1)
        return line + b"\n"

    def listening(self, app_id):
        """Opens a window that says when a button's pressed on it, once it
        has the pointer, and returns its id and client."""
        client = self.wm.open_window(app_id, app_id, stdin=True, stdout=True, scrolls=True)
        con = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                                  and e["container"]["app_id"] == app_id)["container"]
        self.pointer("move", "0", "0", *self.size())
        while self.line(client) != b"pointing\n":
            pass
        return con["id"], client

    def said(self, client, *lines):
        """What client says next, until its pong, which comes after all it
        had to say before it was asked."""
        client.stdin.write(b"ping\n")
        client.stdin.flush()
        heard = []
        while (line := self.line(client)) != b"pong\n":
            heard.append(line)
        return heard


class TileTest(SplitTest):
    def test_dragging_the_edge_moves_it_with_the_pointer(self):
        x, y, w, h = self.area()
        master, stack = self.windows_in("tile", 2, 0.5)
        edge = x + w // 2
        self.drag(edge, y + h // 2, edge + 128)
        self.assertEqual(self.boxes([master, stack]),
                         [(x, y, w // 2 + 128, h), (edge + 128, y, w - w // 2 - 128, h)])
        self.drag(edge + 128, y + h // 2, edge - 64)
        self.assertEqual(self.box(master)[2], w // 2 - 64)

    def test_the_edge_keeps_its_distance_from_the_pointer(self):
        x, y, w, h = self.area()
        master, _ = self.windows_in("tile", 2, 0.5)
        edge = x + w // 2
        self.drag(edge + 3, y + h // 2, edge + 3 + 64)
        self.assertEqual(self.box(master)[2], w // 2 + 64)
        self.drag(edge + 64 - 4, y + h // 2, edge - 4)
        self.assertEqual(self.box(master)[2], w // 2)

    def test_it_stays_between_a_tenth_and_nine_tenths(self):
        x, y, w, h = self.area()
        master, _ = self.windows_in("tile", 2, 0.5)
        self.drag(x + w // 2, y + h // 2, x + 1)
        self.assertEqual(self.box(master)[2], round(w * 0.1))

    def test_a_press_beyond_the_grab_goes_to_the_window(self):
        x, y, w, h = self.area()
        self.command(self.connect(), "layout tile")
        self.command(self.connect(), "mfact 0.5")
        master, client = self.listening("test.master")
        self.open_window(self.ipc, "test.stack")
        before = self.box(master)
        edge = x + w // 2
        self.drag(edge - 5, y + h // 2, edge + 100)
        self.assertEqual(self.box(master), before)
        self.assertIn(b"button\n", self.said(client))

    def test_the_window_never_sees_a_press_on_the_edge(self):
        x, y, w, h = self.area()
        self.command(self.connect(), "layout tile")
        self.command(self.connect(), "mfact 0.5")
        master, client = self.listening("test.master")
        self.open_window(self.ipc, "test.stack")
        edge = x + w // 2
        self.drag(edge - 2, y + h // 2, edge + 98)  # the edge keeps its 2 px
        self.assertEqual(self.box(master)[2], w // 2 + 100)
        self.assertNotIn(b"button\n", self.said(client))

    def test_no_edge_between_a_stacks_rows(self):
        x, y, w, h = self.area()
        ids = self.windows_in("tile", 3, 0.5)
        before = self.boxes(ids)
        row = before[2][1]
        self.drag(x + 3 * w // 4, row, x + 3 * w // 4, row + 100)
        self.drag(x + 3 * w // 4, row, x + w // 4)
        self.assertEqual(self.boxes(ids), before)

    def test_with_only_masters_there_is_no_edge(self):
        x, y, w, h = self.area()
        ids = self.windows_in("tile", 2, 0.5)
        self.command(self.connect(), "nmaster 2")
        before = self.boxes(ids)
        self.drag(x + w // 2, y + h // 2, x + w // 2 + 100)
        self.assertEqual(self.boxes(ids), before)


class OtherLayoutsTest(SplitTest):
    def test_twocols_edge_is_before_the_stack(self):
        x, y, w, h = self.area()
        first, second, stack = self.windows_in("twocol", 3, 0.5)
        edge = self.box(stack)[0]
        self.drag(edge, y + h // 2, edge + 128)
        self.assertEqual(self.box(stack)[0], edge + 128)
        self.assertEqual(self.box(second)[0] + self.box(second)[2], edge + 128)

    def test_no_edge_between_twocols_masters(self):
        x, y, w, h = self.area()
        ids = self.windows_in("twocol", 3, 0.5)
        before = self.boxes(ids)
        between = before[1][0]
        self.drag(between, y + h // 2, between + 100)
        self.assertEqual(self.boxes(ids), before)

    def test_threecols_master_grows_both_ways_from_either_side(self):
        x, y, w, h = self.area()
        master, _, _ = self.windows_in("threecol", 3)
        left = self.box(master)[0]
        self.drag(left, y + h // 2, left - 64)
        self.assertEqual(self.box(master), (left - 64, y, w - 2 * (left - 64 - x), h))
        right = left - 64 + self.box(master)[2]
        self.drag(right, y + h // 2, right - 128)
        self.assertEqual(self.box(master), (left + 64, y, w - 2 * (left + 64 - x), h))

    def test_threecol_with_one_stack_window_drags_as_tile(self):
        x, y, w, h = self.area()
        master, other = self.windows_in("threecol", 2)
        edge = self.box(other)[0]
        self.drag(edge, y + h // 2, edge + 64)
        self.assertEqual(self.boxes([master, other]),
                         [(x, y, edge + 64 - x, h), (edge + 64, y, x + w - edge - 64, h)])

    def test_monocle_has_no_edge(self):
        x, y, w, h = self.area()
        ids = self.windows_in("monocle", 2)
        before = self.boxes(ids)
        self.drag(x + w // 2, y + h // 2, x + w // 2 + 100)
        self.assertEqual(self.boxes(ids), before)


class LoneWindowTest(SplitTest):
    def setUp(self):
        super().setUp()
        self.command(self.connect(), "output HEADLESS-1 mode 2560x1080")

    def test_a_lone_windows_sides_drag_its_width_centered(self):
        x, y, w, h = self.area()
        con, = self.windows_in("tile", 1)
        self.assertEqual(self.box(con), (x + w // 10, y, w * 8 // 10, h))  # 80%
        right = x + w * 9 // 10
        self.drag(right, y + h // 2, right + w // 20)
        self.assertEqual(self.box(con), (x + w // 20, y, w * 9 // 10, h))
        self.drag(x + w // 20, y + h // 2, x + w // 4)
        self.assertEqual(self.box(con), (x + w // 4, y, w // 2, h))

    def test_the_width_is_the_parts_own_whenever_its_alone(self):
        x, y, w, h = self.area()
        con, = self.windows_in("tile", 1)
        right = x + w * 9 // 10
        self.drag(right, y + h // 2, right - w // 10)
        alone = (x + w // 5, y, w * 6 // 10, h)
        self.assertEqual(self.box(con), alone)
        client, other = self.open_window(self.ipc, "test.other")
        self.assertEqual(self.box(con)[0], x)  # tiled as ever
        client.kill()
        self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "close")
        self.assertEqual(self.box(con), alone)
        self.command(self.connect(), "single_width reset")
        self.assertEqual(self.box(con), (x + w // 10, y, w * 8 // 10, h))

    def test_single_width_sets_it(self):
        x, y, w, h = self.area()
        con, = self.windows_in("tile", 1)
        self.command(self.connect(), "single_width 0.5")
        self.assertEqual(self.box(con), (x + w // 4, y, w // 2, h))
        for bad in ["single_width 0.05", "single_width 2", "single_width +0.1",
                    "single_width wide"]:
            reply, = self.connect().request(RUN_COMMAND, bad)
            self.assertFalse(reply["success"], bad)

    def test_a_press_that_doesnt_move_sets_no_width(self):
        x, y, w, h = self.area()
        con, = self.windows_in("tile", 1)
        right = x + w * 9 // 10
        self.drag(right, y + h // 2, right)
        self.command(self.connect(), "output HEADLESS-1 mode 3840x1080")
        x, y, w, h = self.area()
        self.assertEqual(self.box(con), (x + w // 5, y, w * 6 // 10, h))  # the rule's 60%


class PassesOnTest(SplitTest):
    """Where the press goes on to what's there, as it would without the
    edge."""
    CONFIG = FLOAT

    def assert_still(self, ids, x, y, to_x):
        before = self.boxes(ids)
        self.drag(x, y, to_x)
        self.assertEqual(self.boxes(ids), before)

    def test_over_a_floating_window(self):
        x, y, w, h = self.area()
        ids = self.windows_in("tile", 2, 0.5)
        floating = self.open_window(self.ipc, "test.float")[1]["id"]
        fx, fy, fw, fh = self.box(floating)
        edge = x + w // 2
        self.assertTrue(fx <= edge < fx + fw)
        self.assert_still(ids, edge, fy + fh // 2, edge + 100)
        self.drag(edge, fy + fh + 10, edge + 100)  # below it, the edge drags
        self.assertEqual(self.box(ids[0])[2], w // 2 + 100)

    def test_over_a_top_layer(self):
        x, y, w, h = self.area()
        ids = self.windows_in("tile", 2, 0.9)
        edge = self.box(ids[1])[0]
        note = subprocess.Popen([LAUNCHER, "notification"], env=self.wm.env, bufsize=0,
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.wm.clients.append(note)
        self.addCleanup(note.stdout.close)
        self.assertEqual(self.line(note), b"open\n")
        # The notification is 200x100 in the top right corner
        self.assertTrue(x + w - 200 <= edge)
        self.assert_still(ids, edge, y + 50, edge - 100)
        self.drag(edge, y + 150, edge - 100)
        self.assertEqual(self.box(ids[1])[0], edge - 100)

    def test_over_a_windows_popup(self):
        x, y, w, h = self.area()
        self.command(self.connect(), "layout tile")
        self.command(self.connect(), "mfact 0.5")
        master = self.open_window(self.ipc, "test.master")[1]["id"]
        client = self.wm.open_window("test.stack", "test.stack", stdin=True, stdout=True)
        stack = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new")["container"]
        # Its menu is around its top left corner, on the edge
        client.stdin.write(b"menu\n")
        client.stdin.flush()
        self.assertEqual(self.line(client), b"menu\n")
        edge = x + w // 2
        self.assert_still([master, stack["id"]], edge, y + 10, edge + 100)

    def test_while_a_popup_grabs_the_pointer(self):
        x, y, w, h = self.area()
        self.command(self.connect(), "layout tile")
        self.command(self.connect(), "mfact 0.5")
        master, client = self.listening("test.master")
        stack = self.open_window(self.ipc, "test.stack")[1]["id"]
        self.pointer("move", str(x + 100), str(y + h // 2), *self.size(), "click")
        self.assertEqual(self.said(client), [b"button\n"])
        client.stdin.write(b"popup menu\n")
        client.stdin.flush()
        self.assertEqual(self.line(client), b"menu\n")
        edge = x + w // 2
        self.assert_still([master, stack], edge + 2, y + h // 2, edge + 100)
        self.assertIn(b"menu closed\n", self.said(client))

    def test_under_a_maximized_or_fullscreen_window(self):
        x, y, w, h = self.area()
        ids = self.windows_in("tile", 2, 0.5)
        edge = x + w // 2
        for state in ["maximize", "fullscreen"]:
            with self.subTest(state=state):
                self.command(self.connect(), f"[con_id={ids[0]}] {state} enable")
                self.drag(edge, y + h // 2, edge + 100)
                self.command(self.connect(), f"[con_id={ids[0]}] {state} disable")
                self.assertEqual(self.box(ids[0])[2], w // 2)


class TwoMonitorsTest(SplitTest):
    OUTPUTS = 2

    def test_only_on_the_focused_monitor(self):
        self.command(self.connect(), "focus output HEADLESS-2")
        ids = self.windows_in("tile", 2, 0.5)
        x, y, w, h = self.area("HEADLESS-2")
        edge = x + w // 2
        self.command(self.connect(), "focus output HEADLESS-1")
        before = self.boxes(ids)
        self.drag(edge, y + h // 2, edge + 100)
        self.assertEqual(self.boxes(ids), before)
        self.command(self.connect(), "focus output HEADLESS-2")
        self.drag(edge, y + h // 2, edge + 100)
        self.assertEqual(self.box(ids[0])[2], w // 2 + 100)


class GrabTest(SplitTest):
    CONFIG = "resize_grab 10\n"

    def test_resize_grab_widens_the_grab(self):
        x, y, w, h = self.area()
        master, _ = self.windows_in("tile", 2, 0.5)
        edge = x + w // 2
        self.drag(edge + 9, y + h // 2, edge + 9 + 64)
        self.assertEqual(self.box(master)[2], w // 2 + 64)

    def test_bad_values_are_errors(self):
        for text in ["resize_grab -1\n", "resize_grab 33\n", "resize_grab 2.5\n",
                     "resize_grab\n", "mode m {\nresize_grab 4\n}\n"]:
            with self.subTest(text=text):
                self.wm.write_config(text)
                result, = self.connect().request(RUN_COMMAND, "reload")
                self.assertFalse(result["success"], text)
                self.assertIn("resize_grab", result["error"])


class NoGrabTest(SplitTest):
    CONFIG = "resize_grab 0\n"

    def test_zero_turns_the_drag_off(self):
        x, y, w, h = self.area()
        ids = self.windows_in("tile", 2, 0.5)
        before = self.boxes(ids)
        self.drag(x + w // 2, y + h // 2, x + w // 2 + 100)
        self.assertEqual(self.boxes(ids), before)


class SuperRightDragTest(SplitTest):
    CONFIG = "floating_modifier Mod4 normal\n" + HOLD

    def holding_super(self):
        return MouseTest.holding(self, "logo", "super")

    def test_it_drags_a_tiles_nearest_edge(self):
        x, y, w, h = self.area()
        master, stack = self.windows_in("tile", 2, 0.5)
        with self.holding_super():
            self.drag(x + 100, y + h // 2, x + 164, button="right")
        self.assertEqual(self.box(master)[2], w // 2 + 64)
        self.assertFalse(self.windows()[master]["type"] == "floating_con")

    def test_a_tile_with_no_edge_floats(self):
        x, y, w, h = self.area()
        ids = self.windows_in("monocle", 2)
        with self.holding_super():
            self.drag(x + 100, y + 100, x + 300, button="right")
        self.assertIn("floating_con", [self.windows()[i]["type"] for i in ids])


if __name__ == "__main__":
    unittest.main()
