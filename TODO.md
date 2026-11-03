# TODO

tidewm is tide's compositor: dwl with what tide needs from a compositor
added. The requirements, each with a check, are mikelward/tide's
`docs/compositor.md`, and *dwl 0.9 against this list* there says what dwl
already does. Items go in this order, one PR each unless split; a feature
lands with a test.

## Decided

- **IPC** (maintainer, 2026-11-03): sway-compatible. sway's documented
  i3-ipc framing and JSON, the subset tide needs, plus tide's own fields and
  commands, so Quickshell's I3 module, `swaymsg` and waybar's sway modules
  work against it.

## Done

- **IPC and a test harness** (#7): sway's i3-ipc on `SWAYSOCK`
  (`docs/ipc.md`), and `make test`, which runs tidewm headless and asserts
  over it.

## Plan

1. **A shared workspace pool** in place of dwl's per-monitor tags:
   workspaces 1–9, each shown on at most one monitor, with ext-workspace,
   workspace events and the commands to show and move to one.
2. **Runtime config with reload**: bindings (locked and release ones too,
   and submaps), window rules, outputs, per-device input and XKB, from a
   file reread by a `reload` command.
3. **Layouts**: new windows join the end of the stack; three-column, two
   columns + stack and monocle beside tile, set per workspace; the layout
   messages and a layout event.
4. **The focus guard**: no window takes focus on map without a grant;
   grants over the IPC, lapsing on input elsewhere and on a timer; refused
   windows marked urgent, with an attention list and a cycle; activation,
   Wayland and X11, onto the urgent path; the last input and pids for the
   polkit check.
5. **Window states**: maximize, ending when another window on the
   workspace takes focus; fullscreen on open, applied with the grant.
6. **Dialogs and floating**: centered on the parent, capped at 80% of the
   monitor, following it; modal; a picture-in-picture rule; a hand-floated
   window's size remembered.
7. **Dimming** of every window but the focused one, with tide's
   exceptions.
8. **Input**: a tap-Super event; per-device settings and scroll factor.
9. **Outputs**: runtime rules (mode, scale, position, single-window
    width), reapplied on hotplug; the lid switch.
10. **Lock and keyboard**: a command that drops a dead lock, no pointer
    events while locked; on-demand layers keep the keyboard.
11. **Capture**: window identifiers over the IPC; capture start and stop
    events; tide's launcher and notifications hidden from capture.
12. **Outside clicks**: a focus-grab protocol or click event for the
    notification center.
13. **Sharp X11 at fractional scale**: unscaled Xwayland.
14. **Session**: the entry and wrapper tide starts it with.

Then tide's shell gets a tidewm backend beside its Hyprland one.
