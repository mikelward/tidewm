"""The focus guard, tide's SPEC §14 (docs/config.md, docs/ipc.md): with it
enabled, a new window takes focus only when you asked for it, through a
grant, or it's from the app you're in. The rest open unfocused and urgent.
These are tide's hypr/tide/focus_test.lua cases, against tidewm."""

import os
import select
import subprocess
import unittest

from harness import (CLIENT, EVENT_WINDOW, GET_CONFIG, GET_WORKSPACES, ROOT,
                     RUN_COMMAND, TIMEOUT, TidewmTest)

POINTER = os.path.join(ROOT, "tests", "pointer")
LAUNCHER = os.path.join(ROOT, "tests", "launcher")


class GuardTest(TidewmTest):
    CONFIG = "focus_guard enable\n"

    def setUp(self):
        super().setUp()
        self.ipc = self.connect()
        self.ipc.subscribe("window")

    def grant(self, args):
        self.command(self.connect(), f"grant {args}")

    def open(self, app_id, title=None, x11=False):
        """Opens a window, returning its id and its client, whose stdin
        takes "activate"."""
        if x11:
            client = self.wm.open_x11_window(app_id, title or app_id, stdin=True)
            match = lambda e: (e["change"] == "new" and e["container"].get(
                "window_properties", {}).get("class") == app_id)
        else:
            client = self.wm.open_window(app_id, title or app_id, stdin=True, stdout=True)
            match = lambda e: e["change"] == "new" and e["container"]["app_id"] == app_id
        return self.ipc.next_event(EVENT_WINDOW, match)["container"]["id"], client

    def activate(self, client, con_id):
        """Has the window ask to be activated, and waits until tidewm has
        heard: it says so with a focus or urgent event."""
        client.stdin.write(b"activate\n")
        client.stdin.flush()
        self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] in ("focus", "urgent")
                            and e["container"]["id"] == con_id)

    def state(self, con_id):
        """Whether the window is focused, and whether it's urgent."""
        w = self.windows()[con_id]
        return w["focused"], w["urgent"]

    def focused(self):
        return [i for i, w in self.windows().items() if w["focused"]]

    def click(self):
        subprocess.run([POINTER, "click"], env=self.wm.env, check=True, timeout=TIMEOUT)

    def launcher(self):
        """Opens tests/launcher, which holds the keyboard, as tide's
        launcher does, and returns it once it's open."""
        launcher = subprocess.Popen([LAUNCHER], env=self.wm.env, stdin=subprocess.PIPE,
                                    stdout=subprocess.PIPE)
        self.wm.clients.append(launcher)
        self.addCleanup(launcher.stdout.close)
        self.assertEqual(self.line(launcher), b"open\n")
        return launcher

    def close_launcher(self, launcher):
        launcher.stdin.write(b"close\n")
        launcher.stdin.flush()
        self.assertEqual(self.line(launcher), b"closed\n")

    def line(self, process):
        self.assertTrue(select.select([process.stdout], [], [], TIMEOUT)[0], "it said nothing")
        return process.stdout.readline()

    def fails(self, command):
        result, = self.connect().request(RUN_COMMAND, command)
        self.assertFalse(result["success"], f"{command!r}: {result}")
        return result["error"]


class OpenTest(GuardTest):
    def test_a_window_no_grant_asked_for_opens_unfocused_and_urgent(self):
        self.grant("test.first")
        first, _ = self.open("test.first")
        other, _ = self.open("test.other")
        self.assertEqual(self.state(other), (False, True))
        self.assertEqual(self.state(first), (True, False))
        self.assertTrue(self.windows()[other]["visible"])

    def test_with_nothing_focused_only_a_granted_window_takes_focus(self):
        held, _ = self.open("test.held")
        self.assertEqual((self.state(held), self.focused()), ((False, True), []))
        self.grant("test.granted")
        granted, _ = self.open("test.granted")
        self.assertEqual(self.focused(), [granted])

    def test_a_held_window_says_its_urgent(self):
        self.open("test.held")
        event = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "urgent")
        self.assertEqual(event["container"]["app_id"], "test.held")

    def test_a_new_window_from_the_app_youre_in_takes_focus(self):
        self.grant("test.app")
        self.open("test.app", "One")
        second, _ = self.open("test.app", "Two")
        self.assertEqual(self.state(second), (True, False))

    def test_a_status_popup_stays_unfocused_and_unmarked(self):
        # No app ID and no title, as Chrome's own notification opens
        self.grant("test.app")
        app, _ = self.open("test.app")
        popup, _ = self.open("", "")
        self.assertEqual(self.state(popup), (False, False))
        self.assertEqual(self.focused(), [app])

    def test_a_status_popup_takes_no_grant(self):
        self.grant("*")
        popup, _ = self.open("", "")
        self.assertEqual(self.state(popup), (False, False))
        window, _ = self.open("test.app")
        self.assertEqual(self.focused(), [window])

    def test_focus_next_and_prev_pass_a_status_popup_by(self):
        self.grant("test.one")
        one, _ = self.open("test.one")
        self.grant("test.two")
        two, _ = self.open("test.two")
        popup, _ = self.open("", "")
        for _ in range(3):
            self.command(self.connect(), "focus next")
            self.assertNotEqual(self.focused(), [popup])
        for _ in range(3):
            self.command(self.connect(), "focus prev")
            self.assertNotEqual(self.focused(), [popup])
        self.assertEqual(self.state(popup), (False, False))

    def test_zoom_passes_a_status_popup_by(self):
        self.grant("test.master")
        master, _ = self.open("test.master")
        popup, _ = self.open("", "")
        self.command(self.connect(), "zoom")
        self.assertEqual((self.focused(), self.state(popup)), ([master], (False, False)))

    def test_a_window_with_a_title_but_no_app_id_is_no_status_popup(self):
        self.grant("test.app")
        self.open("test.app")
        titled, _ = self.open("", "Titled")
        self.assertEqual(self.state(titled), (False, True))


class GrantTest(GuardTest):
    def test_the_first_window_of_a_granted_app_takes_focus(self):
        self.grant("test.app")
        window, _ = self.open("test.app")
        self.assertEqual(self.state(window), (True, False))

    def test_a_grant_names_an_app_by_case_and_desktop_id_alike(self):
        self.grant("Org.Example.Files.desktop")
        window, _ = self.open("org.example.Files")
        self.assertEqual(self.focused(), [window])

    def test_a_bare_name_matches_a_qualified_id(self):
        self.grant("files")
        window, _ = self.open("org.example.Files")
        self.assertEqual(self.focused(), [window])

    def test_a_grant_for_one_namespace_isnt_used_by_another(self):
        self.grant("org.example.chat")
        window, _ = self.open("com.example.chat")
        self.assertEqual(self.state(window), (False, True))

    def test_a_grant_listing_several_apps_is_used_by_a_window_of_any(self):
        self.grant("test.one test.two")
        window, _ = self.open("test.two")
        self.assertEqual(self.focused(), [window])

    def test_a_grant_is_used_once(self):
        self.grant("*")
        first, _ = self.open("test.first")
        other, _ = self.open("test.other")
        self.assertEqual((self.focused(), self.state(other)), ([first], (False, True)))

    def test_a_named_grant_is_used_before_a_wildcard(self):
        self.grant("*")
        self.grant("test.named")
        self.open("test.named")
        # The wildcard's still there for the next app's window
        other, _ = self.open("test.other")
        self.assertEqual(self.focused(), [other])

    def test_one_grants_window_leaves_the_other_grants(self):
        self.grant("test.one")
        self.grant("test.two")
        self.open("test.one")
        two, _ = self.open("test.two")
        self.assertEqual(self.focused(), [two])

    def test_a_window_whose_process_descends_from_a_grants_pid_takes_focus(self):
        # The test's own process stands for a terminal's shell: its windows
        # are its children
        self.grant(f"--pid {os.getpid()}")
        window, _ = self.open("test.from-shell")
        self.assertEqual(self.focused(), [window])

    def test_a_window_from_elsewhere_doesnt_take_a_pids_grant(self):
        other = subprocess.Popen(["sleep", "60"])
        self.addCleanup(other.wait)
        self.addCleanup(other.kill)
        self.grant(f"--pid={other.pid}")
        window, _ = self.open("test.elsewhere")
        self.assertEqual(self.state(window), (False, True))

    def test_a_grant_on_a_workspace_takes_its_window_there_with_focus(self):
        self.grant("--workspace two test.app")
        window, _ = self.open("test.app")
        self.assertEqual(self.windows()[window]["workspace"], "two")
        self.assertEqual(self.focused(), [window])
        self.assertEqual([w["name"] for w in self.connect().request(GET_WORKSPACES) if w["focused"]],
                         ["two"])

    def test_going_to_a_grants_workspace_leaves_the_other_grants(self):
        self.grant("--workspace=two test.app")
        self.grant("test.next")
        self.open("test.app")
        window, _ = self.open("test.next")
        self.assertEqual(self.focused(), [window])

    def test_extend_widens_a_grant_that_still_holds(self):
        self.grant("--key 7 test.prog")
        self.command(self.connect(), "grant extend --key=7 test.prog org.example.App")
        window, _ = self.open("org.example.App")
        self.assertEqual(self.focused(), [window])

    def test_extend_widens_only_the_grant_its_key_names(self):
        self.grant("--key 7 test.prog")
        self.command(self.connect(), "grant extend --key 8 test.prog org.example.App")
        window, _ = self.open("org.example.App")
        self.assertEqual(self.state(window), (False, True))

    def test_the_grant_command_rejects_what_isnt_a_grant(self):
        for command, error in [
            ("grant", "grant needs an app id, *, or --pid"),
            ("grant * test.app", "* stands alone"),
            ("grant --pid 1", "--pid takes a process id"),
            ("grant --pid=abc test.app", "--pid takes a process id"),
            ("grant --key 0 test.app", "--key takes a positive whole number"),
            ("grant --workspace", "--workspace takes a workspace's name"),
            ("grant --color red test.app", "--color is not an option"),
            ('grant "test.app', "expected app ids"),
            ("grant extend test.prog org.example.App", "grant extend needs --key"),
            ("grant extend --key 7 test.prog", "grant extend needs --key"),
            ("grant extend --key 7 --workspace two test.prog x", "--workspace is not an option"),
        ]:
            self.assertIn(error, self.fails(command), command)


class CancelTest(GuardTest):
    def test_a_key_press_cancels_the_grant(self):
        self.grant("test.app")
        self.press("x")
        window, _ = self.open("test.app")
        self.assertEqual(self.state(window), (False, True))

    def test_a_click_cancels_the_grant_even_in_the_window_youre_in(self):
        self.grant("test.first")
        first, _ = self.open("test.first")
        self.grant("test.late")
        self.click()  # the pointer is over test.first, the only window
        self.assertEqual(self.focused(), [first])
        late, _ = self.open("test.late")
        self.assertEqual(self.state(late), (False, True))

    def test_moving_focus_cancels_the_grant(self):
        self.grant("test.one")
        one, _ = self.open("test.one")
        self.grant("test.two")
        self.open("test.two")
        self.grant("test.late")
        self.command(self.connect(), f"[con_id={one}] focus")
        late, _ = self.open("test.late")
        self.assertEqual(self.state(late), (False, True))

    def test_a_workspace_switch_cancels_the_grant(self):
        self.grant("test.app")
        self.command(self.connect(), "workspace two")
        window, _ = self.open("test.app")
        self.assertEqual(self.state(window), (False, True))

    def test_closing_the_window_youre_in_cancels_the_grant(self):
        self.grant("test.one")
        _, client = self.open("test.one")
        self.grant("test.late")
        client.kill()
        self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "close")
        late, _ = self.open("test.late")
        self.assertEqual(self.state(late), (False, True))

    def test_the_launcher_closing_doesnt_cancel_the_grant(self):
        # Focus comes back to the window you were in
        self.grant("test.first")
        first, _ = self.open("test.first")
        launcher = self.launcher()
        self.grant("test.app")
        self.close_launcher(launcher)
        self.assertEqual(self.focused(), [first])
        window, _ = self.open("test.app")
        self.assertEqual(self.focused(), [window])

    def test_the_launcher_closing_on_an_empty_workspace_doesnt_cancel_the_grant(self):
        launcher = self.launcher()
        self.grant("test.app")
        self.close_launcher(launcher)
        window, _ = self.open("test.app")
        self.assertEqual(self.focused(), [window])

    def test_a_window_granted_while_the_launcher_is_open_leaves_the_other_grants(self):
        # Its focus waits for the launcher to close, and is still the grant's
        launcher = self.launcher()
        self.grant("test.first")
        self.grant("test.second")
        first, _ = self.open("test.first")
        self.close_launcher(launcher)
        self.assertEqual(self.focused(), [first])
        second, _ = self.open("test.second")
        self.assertEqual(self.focused(), [second])

    def test_a_granted_activation_while_the_launcher_is_open_leaves_the_other_grants(self):
        self.grant("test.a")
        a, client = self.open("test.a")
        self.grant("test.x")
        self.open("test.x")
        launcher = self.launcher()
        self.grant("test.a")
        self.grant("test.b")
        client.stdin.write(b"activate\n")
        client.stdin.flush()
        self.assertEqual(self.line(client), b"activated\n")
        self.close_launcher(launcher)
        self.assertEqual(self.focused(), [a])
        b, _ = self.open("test.b")
        self.assertEqual(self.focused(), [b])

    def test_going_from_an_empty_workspace_to_another_cancels_the_grant(self):
        self.grant("test.app")
        self.command(self.connect(), "workspace two")
        self.command(self.connect(), "workspace three")
        window, _ = self.open("test.app")
        self.assertEqual(self.state(window), (False, True))

    def test_focusing_a_window_no_grant_asked_for_cancels_the_grants(self):
        # A dialog from the app you're in takes focus, and moves you on
        self.grant("test.app")
        self.open("test.app", "Main")
        self.grant("test.late")
        self.open("test.app", "Dialog")
        late, _ = self.open("test.late")
        self.assertEqual(self.state(late), (False, True))


class BindingTest(GuardTest):
    CONFIG = f"focus_guard enable\nbindsym x grant test.bound; exec {CLIENT} test.bound Bound\n"

    def test_a_binding_that_grants_its_app_first_gives_it_focus(self):
        # The key press moves you on before the binding grants
        self.press("x")
        window = self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new")["container"]
        self.assertEqual(self.state(window["id"]), (True, False))


class RuleTest(GuardTest):
    CONFIG = 'focus_guard enable\nfor_window [title="Elsewhere"] move to workspace three\n'

    def test_a_window_of_your_app_opening_elsewhere_leaves_the_grants(self):
        # It doesn't take focus, so you haven't moved on
        self.grant("test.app")
        self.open("test.app", "Main")
        self.grant("test.other")
        elsewhere, _ = self.open("test.app", "Elsewhere")
        self.assertEqual(self.windows()[elsewhere]["workspace"], "three")
        other, _ = self.open("test.other")
        self.assertEqual(self.focused(), [other])


class NotificationTest(GuardTest):
    """A notification's click grants its app, which, if the grant lapses
    unused, brings up the app's most recent window (tide's SPEC §9). The
    lapse is short, so the tests focus their windows by id, not grants."""

    CONFIG = ('focus_guard enable\nfocus_guard grant_seconds 0.05\n'
              'for_window [title="Elsewhere"] move to workspace three\n')

    def focus(self, con_id):
        """Focuses the window, and takes its focus event, so later waits
        see only what follows."""
        self.command(self.connect(), f"[con_id={con_id}] focus")
        self.focused_on(con_id)

    def focused_on(self, con_id):
        """Waits for the window's focus event."""
        self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "focus"
                            and e["container"]["id"] == con_id)

    def focus_events_until(self, con_id):
        """The windows focused, in order, until con_id is."""
        seen = []

        def record(e):
            if e["change"] == "focus":
                seen.append(e["container"]["id"])
            return e["change"] == "focus" and e["container"]["id"] == con_id
        self.ipc.next_event(EVENT_WINDOW, record)
        return seen

    def test_a_lapsed_notification_grant_brings_up_the_apps_most_recent_window(self):
        first, _ = self.open("test.a", "First")
        second, _ = self.open("test.a", "Second")
        other, _ = self.open("test.x")
        for con in (second, first, other):
            self.focus(con)
        self.grant("--or-recent test.a")
        self.focused_on(first)
        self.assertEqual(self.focused(), [first])

    def test_with_none_ever_focused_any_of_the_apps_windows_comes_up(self):
        window, _ = self.open("test.a")
        other, _ = self.open("test.x")
        self.focus(other)
        self.grant("--or-recent test.a")
        self.focused_on(window)
        self.assertEqual(self.focused(), [window])

    def test_a_notification_grant_can_name_several_apps(self):
        one, _ = self.open("test.one")
        two, _ = self.open("test.two")
        other, _ = self.open("test.x")
        for con in (one, two, other):
            self.focus(con)
        self.grant("--or-recent test.one test.two")
        self.focused_on(two)
        self.assertEqual(self.focused(), [two])

    # In the next two, a later notification's grant is a clock: lapses
    # come in order, so a wrong one would show up before it

    def test_a_notification_grant_a_new_window_used_brings_nothing_up(self):
        # The window uses the grant but opens elsewhere, unfocused, so a
        # fallback would show as its focus
        other, _ = self.open("test.x")
        clock, _ = self.open("test.y")
        self.focus(other)
        self.grant("--or-recent test.a")
        window, _ = self.open("test.a", "Elsewhere")
        self.assertEqual(self.windows()[window]["workspace"], "three")
        self.grant("--or-recent test.y")
        self.assertEqual(self.focus_events_until(clock), [clock])

    def test_a_notification_grant_you_moved_on_from_brings_nothing_up(self):
        window, _ = self.open("test.a")
        other, _ = self.open("test.x")
        clock, _ = self.open("test.y")
        self.focus(window)
        self.focus(other)
        self.grant("--or-recent test.a")
        self.press("x")
        self.grant("--or-recent test.y")
        self.assertEqual(self.focus_events_until(clock), [clock])

    def test_an_app_with_no_window_gets_nothing_and_its_said(self):
        other, _ = self.open("test.x")
        self.focus(self.open("test.y")[0])
        self.grant("--or-recent test.none")
        self.grant("--or-recent test.x")
        self.focused_on(other)
        self.wm.log.seek(0)
        self.assertIn("no window of test.none to bring up", self.wm.log.read())

    def test_focus_recent_brings_up_the_apps_most_recent_window_at_once(self):
        first, _ = self.open("test.a", "First")
        second, _ = self.open("test.a", "Second")
        other, _ = self.open("test.x")
        for con in (first, other):
            self.focus(con)
        self.command(self.connect(), "focus recent test.a")
        self.assertEqual(self.focused(), [first])

    def test_focus_recent_is_you_moving_on(self):
        window, _ = self.open("test.a")
        self.grant("test.late")
        self.command(self.connect(), "focus recent test.a")
        late, _ = self.open("test.late")
        self.assertEqual(self.state(late), (False, True))

    def test_focus_recent_with_no_window_fails(self):
        self.assertIn("No window of test.none", self.fails("focus recent test.none"))

    def test_the_commands_reject_what_they_cant_take(self):
        for command, error in [
            ("grant --or-recent", "grant needs an app id"),
            ("grant --or-recent *", "--or-recent takes app ids alone"),
            (f"grant --or-recent --pid {os.getpid()} test.a", "--or-recent takes app ids alone"),
            ("grant --or-recent --workspace two test.a", "--or-recent takes app ids alone"),
            ("focus recent", "focus recent needs an app id"),
            ("focus recent *", "focus recent: expected app ids"),
        ]:
            self.assertIn(error, self.fails(command), command)


class ReloadedLapseTest(GuardTest):
    CONFIG = "focus_guard enable\nfocus_guard grant_seconds 30\n"

    def test_a_reload_moves_a_notification_grants_lapse(self):
        window, _ = self.open("test.a")
        other, _ = self.open("test.x")
        self.command(self.connect(), f"[con_id={other}] focus")
        self.grant("--or-recent test.a")
        self.wm.write_config("focus_guard enable\nfocus_guard grant_seconds 0.05\n")
        self.command(self.connect(), "reload")
        self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "focus"
                            and e["container"]["id"] == window)


class LapseTest(GuardTest):
    # A microsecond: the window maps after the grant, from a process
    # started after it, which always takes longer
    CONFIG = "focus_guard enable\nfocus_guard grant_seconds 0.000001\n"

    def test_a_grant_lapses_after_grant_seconds(self):
        self.grant("test.app")
        window, _ = self.open("test.app")
        self.assertEqual(self.state(window), (False, True))


class ExceptionTest(GuardTest):
    def test_a_polkit_prompt_right_after_a_key_press_takes_focus(self):
        self.grant("test.app")
        self.open("test.app")
        self.press("x")
        prompt, _ = self.open("hyprpolkitagent")
        self.assertEqual(self.focused(), [prompt])

    def test_a_recent_key_press_lets_no_other_window_in(self):
        self.press("x")
        window, _ = self.open("test.app")
        self.assertEqual(self.state(window), (False, True))

    def test_a_portal_dialog_takes_focus_while_a_window_has_it(self):
        self.grant("test.app")
        self.open("test.app")
        dialog, _ = self.open("xdg-desktop-portal-gtk")
        self.assertEqual(self.focused(), [dialog])

    def test_a_portal_dialog_with_nothing_focused_waits(self):
        dialog, _ = self.open("xdg-desktop-portal-gtk")
        self.assertEqual(self.state(dialog), (False, True))


class SlowPromptTest(GuardTest):
    CONFIG = "focus_guard enable\nfocus_guard prompt_seconds 0.000001\n"

    def test_a_polkit_prompt_long_after_the_last_key_press_waits(self):
        self.press("x")
        prompt, _ = self.open("hyprpolkitagent")
        self.assertEqual(self.state(prompt), (False, True))


class HeldTest(GuardTest):
    def test_going_to_a_held_windows_workspace_focuses_it(self):
        self.command(self.connect(), "persist workspace 1")
        held, _ = self.open("test.held")
        self.command(self.connect(), "workspace two")
        self.command(self.connect(), "workspace 1")
        self.assertEqual(self.state(held), (True, False))

    def test_a_held_window_waits_when_the_focused_one_closes(self):
        self.grant("test.one")
        _, client = self.open("test.one")
        held, _ = self.open("test.held")
        client.kill()
        self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "close")
        self.assertEqual((self.state(held), self.focused()), ((False, True), []))

    def test_focusing_one_held_window_leaves_the_others_marked(self):
        # As the shell's Attention tab goes to one
        self.command(self.connect(), "persist workspace 1")
        first, _ = self.open("test.first")
        second, _ = self.open("test.second")
        self.command(self.connect(), "workspace two")
        self.command(self.connect(), f"[con_id={first}] focus")
        self.assertEqual((self.state(first), self.state(second)), ((True, False), (False, True)))

    def test_a_grant_for_a_held_windows_activation_leaves_the_others_marked(self):
        self.command(self.connect(), "persist workspace 1")
        first, client = self.open("test.first")
        second, _ = self.open("test.second")
        self.command(self.connect(), "workspace two")
        self.grant("test.first")
        self.activate(client, first)
        self.assertEqual((self.state(first), self.state(second)), ((True, False), (False, True)))

    def test_a_held_window_mapping_again_with_a_grant_takes_focus(self):
        held, client = self.open("test.app")
        self.grant("test.app")
        client.stdin.write(b"remap\n")
        client.stdin.flush()
        self.ipc.next_event(EVENT_WINDOW, lambda e: e["change"] == "new"
                            and e["container"]["id"] == held)
        self.assertEqual(self.state(held), (True, False))

    def test_a_granted_activation_while_locked_takes_focus_once_unlocked(self):
        self.grant("test.x")
        self.open("test.x")
        held, client = self.open("test.held")
        locker = self.lock()
        self.grant("test.held")
        client.stdin.write(b"activate\n")
        client.stdin.flush()
        self.assertEqual(self.line(client), b"activated\n")
        self.unlock(locker)
        self.assertEqual(self.state(held), (True, False))

    def test_focus_next_reaches_a_held_window(self):
        held, _ = self.open("test.held")
        self.command(self.connect(), "focus next")
        self.assertEqual(self.state(held), (True, False))


class FocusedActivationTest(GuardTest):
    def test_the_focused_windows_activation_uses_its_grant(self):
        self.grant("test.app")
        window, client = self.open("test.app")
        self.grant("--workspace two test.app")
        client.stdin.write(b"activate\n")
        client.stdin.flush()
        self.assertEqual(self.line(client), b"activated\n")
        # So the app's next window doesn't take it, and stays here
        other, _ = self.open("test.app", "Other")
        self.assertEqual((self.windows()[other]["workspace"], self.focused()), ("1", [other]))


class TwoOutputHeldTest(GuardTest):
    OUTPUTS = 2

    def test_going_to_an_output_with_only_a_held_window_focuses_it(self):
        self.command(self.connect(), "focus output HEADLESS-2")
        held, _ = self.open("test.held")
        self.assertEqual(self.windows()[held]["output"], "HEADLESS-2")
        self.command(self.connect(), "focus output HEADLESS-1")
        self.command(self.connect(), "focus output HEADLESS-2")
        self.assertEqual(self.state(held), (True, False))


class OutputChangeTest(GuardTest):
    OUTPUTS = 2

    def test_focus_moving_as_an_output_goes_leaves_the_grants(self):
        # Going away, HEADLESS-2 leaves nothing focused for HEADLESS-1's window
        self.command(self.connect(), "focus output HEADLESS-1")
        self.grant("test.one")
        one, _ = self.open("test.one")
        self.assertEqual(self.windows()[one]["output"], "HEADLESS-1")
        self.command(self.connect(), "focus output HEADLESS-2")
        self.assertEqual(self.focused(), [])
        self.grant("test.app")
        self.command(self.connect(), "output HEADLESS-2 disable")
        self.assertEqual(self.focused(), [one])
        window, _ = self.open("test.app")
        self.assertEqual(self.focused(), [window])


class ActivationTest(GuardTest):
    def two_windows(self):
        self.grant("test.one")
        one, client = self.open("test.one")
        self.grant("test.two")
        two, _ = self.open("test.two")
        return one, client, two

    def test_an_activation_without_a_grant_marks_the_window(self):
        one, client, two = self.two_windows()
        self.activate(client, one)
        self.assertEqual((self.state(one), self.focused()), ((False, True), [two]))

    def test_a_grant_lets_an_activation_take_focus(self):
        one, client, _ = self.two_windows()
        self.grant("test.one")
        self.activate(client, one)
        self.assertEqual(self.state(one), (True, False))

    def test_an_x11_activation_marks_the_window(self):
        self.need(self.wm.x11, "X11 windows need tidewm built with XWAYLAND")
        self.grant("test.x11")
        x11, client = self.open("test.x11", x11=True)
        self.grant("test.two")
        two, _ = self.open("test.two")
        self.activate(client, x11)
        self.assertEqual((self.state(x11), self.focused()), ((False, True), [two]))
        self.grant("test.x11")
        self.activate(client, x11)
        self.assertEqual(self.state(x11), (True, False))


class NoGuardTest(GuardTest):
    CONFIG = None

    def test_without_the_guard_every_new_window_takes_focus(self):
        self.open("test.one")
        two, _ = self.open("test.two")
        self.assertEqual(self.state(two), (True, False))

    def test_without_the_guard_a_grants_workspace_still_takes_its_window_there(self):
        self.grant("--workspace two test.app")
        window, _ = self.open("test.app")
        self.assertEqual((self.windows()[window]["workspace"], self.focused()), ("two", [window]))

    def test_without_the_guard_a_window_still_uses_its_grant(self):
        # So it's left for no activation after
        self.grant("test.one")
        one, client = self.open("test.one")
        self.open("test.two")
        self.activate(client, one)
        self.assertEqual(self.state(one), (False, True))

    def test_without_the_guard_a_new_window_taking_focus_cancels_the_grants(self):
        self.grant("--workspace two test.a")
        self.open("test.b")
        window, _ = self.open("test.a")
        self.assertEqual(self.windows()[window]["workspace"], "1")

    def test_without_the_guard_a_grant_still_lets_an_activation_in(self):
        one, client = self.open("test.one")
        self.open("test.two")
        self.grant("test.one")
        self.activate(client, one)
        self.assertEqual(self.state(one), (True, False))


class ConfigTest(GuardTest):
    def test_focus_guard_lines_are_checked(self):
        for text, error in [
            ("focus_guard maybe\n", ":1: focus_guard takes enable, disable, grant_seconds"),
            ("focus_guard enable now\n", ":1: focus_guard takes enable, disable"),
            ("focus_guard grant_seconds 0\n",
             ":1: focus_guard grant_seconds takes a number of seconds"),
            ("focus_guard prompt_seconds nan\n",
             ":1: focus_guard prompt_seconds takes a number of seconds"),
            ("focus_guard grant_seconds\n", ":1: focus_guard grant_seconds takes a number"),
            ("mode m {\nfocus_guard enable\n}\n", ":2: focus_guard can't be in a mode"),
        ]:
            self.wm.write_config(text)
            result, = self.connect().request(RUN_COMMAND, "reload")
            self.assertFalse(result["success"], text)
            self.assertIn(error, result["error"], text)
        self.assertEqual(self.connect().request(GET_CONFIG)["config"], self.CONFIG)

    def test_disabling_the_guard_on_reload_lets_new_windows_in(self):
        self.wm.write_config("focus_guard disable\n")
        self.command(self.connect(), "reload")
        window, _ = self.open("test.app")
        self.assertEqual(self.state(window), (True, False))


if __name__ == "__main__":
    unittest.main()
