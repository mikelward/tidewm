"""tidewm's IPC: the socket, its requests and its window events."""

import os
import struct
import unittest

from harness import (EVENT_SHUTDOWN, EVENT_TICK, EVENT_WINDOW, GET_OUTPUTS,
                     GET_TREE, GET_VERSION, RUN_COMMAND, SEND_TICK, SUBSCRIBE,
                     TIMEOUT, TidewmTest)


class IpcTest(TidewmTest):
    def test_socket_is_named_in_swaysock(self):
        self.assertTrue(os.path.basename(self.wm.socket).startswith("tidewm-ipc."))
        self.assertEqual(os.path.dirname(self.wm.socket), self.wm.dir.name)

    def test_version_names_tidewm(self):
        version = self.connect().request(GET_VERSION)
        self.assertEqual(version["variant"], "tidewm")
        self.assertTrue(version["human_readable"].startswith("tidewm "))
        for field in ("major", "minor", "patch"):
            self.assertIsInstance(version[field], int)

    def test_outputs_list_the_headless_output(self):
        outputs = self.connect().request(GET_OUTPUTS)
        self.assertEqual(len(outputs), 1)
        output = outputs[0]
        self.assertEqual(output["name"], "HEADLESS-1")
        self.assertTrue(output["active"])
        self.assertTrue(output["focused"])
        self.assertGreater(output["rect"]["width"], 0)
        self.assertGreater(output["current_mode"]["height"], 0)
        self.assertEqual(output["current_workspace"], "1")

    def test_a_window_is_new_then_focused_then_closed(self):
        ipc = self.connect()
        ipc.subscribe("window")
        client, con = self.open_window(ipc, "test.lifecycle", "Lifecycle")
        self.assertEqual(con["name"], "Lifecycle")
        self.assertEqual(con["shell"], "xdg_shell")
        self.assertEqual(con["pid"], client.pid)
        self.assertRegex(con["foreign_toplevel_identifier"], r"^[0-9a-f]{32}$")
        focus = ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "focus")
        self.assertEqual(focus["container"]["id"], con["id"])
        self.assertTrue(focus["container"]["focused"])
        client.kill()
        client.wait(TIMEOUT)
        close = ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "close")
        self.assertEqual(close["container"]["id"], con["id"])

    def test_a_windows_first_event_is_new_once_it_is_placed(self):
        ipc = self.connect()
        ipc.subscribe("window")
        self.wm.open_window("test.placed", "Placed")
        kind, first = ipc.recv()
        self.assertEqual(kind, EVENT_WINDOW)
        self.assertEqual(first["change"], "new")
        self.assertEqual(first["container"]["app_id"], "test.placed")
        self.assertTrue(first["container"]["visible"])
        kind, second = ipc.recv()
        self.assertEqual((kind, second["change"]), (EVENT_WINDOW, "focus"))

    def test_a_window_that_maps_again_is_new_again(self):
        ipc = self.connect()
        ipc.subscribe("window")
        client = self.wm.open_window("test.remap", "Remap", stdin=True)
        new = ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new")
        ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "focus")
        client.stdin.write(b"remap\n")
        client.stdin.flush()
        close = ipc.next_event(EVENT_WINDOW)
        self.assertEqual((close["change"], close["container"]["id"]), ("close", new["container"]["id"]))
        again = ipc.next_event(EVENT_WINDOW)
        self.assertEqual((again["change"], again["container"]["id"]), ("new", new["container"]["id"]))

    def test_an_x11_window_shows_its_class_and_title(self):
        self.need(self.wm.x11, "tidewm wasn't built with XWayland, or has no Xwayland")
        ipc = self.connect()
        ipc.subscribe("window")
        self.wm.open_x11_window("test.x11", "Named")
        new = ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new")
        con = new["container"]
        self.assertEqual(con["shell"], "xwayland")
        self.assertIsNone(con["app_id"])
        self.assertEqual(con["window_properties"]["class"], "test.x11")
        self.assertEqual(con["name"], "Named")

    def test_tree_holds_windows_under_output_and_workspace(self):
        ipc = self.connect()
        ipc.subscribe("window")
        _, first = self.open_window(ipc, "test.first", "First")
        _, second = self.open_window(ipc, "test.second", "Second")
        tree = self.connect().request(GET_TREE)
        self.assertEqual(tree["type"], "root")
        output, = tree["nodes"]
        self.assertEqual(output["type"], "output")
        workspace, = output["nodes"]
        self.assertEqual((workspace["type"], workspace["name"]), ("workspace", "1"))
        windows = {c["id"]: c for c in workspace["nodes"] + workspace["floating_nodes"]}
        self.assertEqual(set(windows), {first["id"], second["id"]})
        self.assertEqual(windows[second["id"]]["app_id"], "test.second")
        self.assertEqual(windows[second["id"]]["workspace"], "1")
        self.assertEqual(windows[second["id"]]["output"], "HEADLESS-1")
        # dwl focuses the newest window
        self.assertTrue(windows[second["id"]]["focused"])
        self.assertFalse(windows[first["id"]]["focused"])

    def test_container_ids_differ_and_stay(self):
        ipc = self.connect()
        ipc.subscribe("window")
        _, first = self.open_window(ipc, "test.a", "A")
        _, second = self.open_window(ipc, "test.b", "B")
        self.assertNotEqual(first["id"], second["id"])
        ids = {c["app_id"]: i for i, c in self.windows().items()}
        self.assertEqual(ids, {"test.a": first["id"], "test.b": second["id"]})

    def test_an_unknown_command_fails(self):
        reply = self.connect().request(RUN_COMMAND, "frobnicate now")
        self.assertEqual(len(reply), 1)
        self.assertFalse(reply[0]["success"])
        self.assertTrue(reply[0]["parse_error"])
        self.assertIn("frobnicate", reply[0]["error"])

    def test_commands_run_in_order(self):
        reply = self.connect().request(RUN_COMMAND, "nop; frobnicate; nop")
        self.assertEqual([r["success"] for r in reply], [True, False, True])

    def test_subscribing_to_an_unknown_event_fails(self):
        reply = self.connect().request(SUBSCRIBE, '["window", "nope"]')
        self.assertFalse(reply["success"])

    def test_subscribing_needs_a_json_array(self):
        reply = self.connect().request(SUBSCRIBE, "window")
        self.assertFalse(reply["success"])

    def test_ticks_reach_subscribers(self):
        listener = self.connect()
        listener.subscribe("tick")
        first = listener.next_event(EVENT_TICK)
        self.assertEqual(first, {"first": True, "payload": ""})
        reply = self.connect().request(SEND_TICK, "hello")
        self.assertTrue(reply["success"])
        tick = listener.next_event(EVENT_TICK)
        self.assertEqual(tick, {"first": False, "payload": "hello"})

    def test_an_unsupported_message_fails_politely(self):
        reply = self.connect().request(5)  # GET_MARKS
        self.assertFalse(reply["success"])

    def test_bad_magic_drops_the_client_only(self):
        bad = self.connect()
        bad.sock.sendall(b"nope!!" + struct.pack("=II", 0, GET_VERSION))
        self.assertEqual(bad.sock.recv(1), b"")
        self.assertEqual(self.connect().request(GET_VERSION)["variant"], "tidewm")

    def test_exit_says_so_then_stops(self):
        listener = self.connect()
        listener.subscribe("shutdown")
        reply = self.connect().request(RUN_COMMAND, "exit")
        self.assertEqual(reply, [{"success": True}])
        self.assertEqual(listener.next_event(EVENT_SHUTDOWN), {"change": "exit"})
        self.assertEqual(self.wm.proc.wait(TIMEOUT), 0)
        self.assertFalse(os.path.exists(self.wm.socket))


if __name__ == "__main__":
    unittest.main()
