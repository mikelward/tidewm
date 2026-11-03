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
- **Config** (maintainer, 2026-11-03): sway's syntax, so a binding's action
  is an IPC command and one parser serves both.

## Done

- **IPC and a test harness** (#7): sway's i3-ipc on `SWAYSOCK`
  (`docs/ipc.md`), and `make test`, which runs tidewm headless and asserts
  over it.
- **Workspaces** (#8): named, made on demand, gone once empty and left
  unless persistent, and spanning every monitor (tide's SPEC.md §6.5), in
  place of dwl's tags, with sway's workspace, move, focus, rename and kill
  commands.
- **ext-workspace** (#9): one group on every output, holding every
  workspace, current one active, urgency shown; a client may activate one.
- **Commands for every default key** (#11): `exec`, `focus next|prev`,
  `floating`, `fullscreen`, `layout`, `nmaster`, `mfact`, `zoom` and
  `chvt`, so a binding can do whatever a key in `config.h` does.

## Decisions needing review

- **Workspaces over the IPC** (#8): `GET_WORKSPACES` lists each workspace
  once, its `output` the focused monitor, since a workspace spans them all;
  the tree lists every workspace under every output, holding that monitor's
  part. The alternative was a sway-style entry per monitor's part, as tide
  names them on Hyprland (`NAME@OUT`). Reversible: only tide's shell reads
  it, and it has no tidewm backend yet.
- **Keeping a workspace** (#8): tidewm's own `persist workspace NAME` and
  `unpersist workspace NAME` commands, for tide's home workspace, since
  sway has none. It could move to the runtime config instead.
- **Layout commands' names** (#11): sway has no master layouts, so
  `layout tile|floating|monocle|toggle`, `nmaster`, `mfact` and `zoom` take
  dwl's names. The layouts item may rename them, before tide's shell uses
  them.
- **exec takes the rest of the line** (#11): `;` included, so it comes
  last, where sway ends it at an unquoted `;`. Finding that `;` means
  reading a shell's quoting, `$(...)` and escapes; this way the shell does
  it. Reversible, if a sway config that chains after `exec` turns up.

## Plan

1. **Runtime config with reload**, from a file reread by a `reload`
   command, in steps:
   1. `bindsym` and `set`, the file, and `reload`;
   2. locked and release bindings, and submaps;
   3. window rules, testing that two matching rules make only the last
      one's workspace (#8);
   4. outputs;
   5. per-device input and XKB.
2. **Layouts**: new windows join the end of the stack; three-column, two
   columns + stack and monocle beside tile, set per workspace; the layout
   messages and a layout event.
3. **The focus guard**: no window takes focus on map without a grant;
   grants over the IPC, lapsing on input elsewhere and on a timer; refused
   windows marked urgent, with an attention list and a cycle; activation,
   Wayland and X11, onto the urgent path; the last input and pids for the
   polkit check.
4. **Window states**: maximize, ending when another window on the
   workspace takes focus; fullscreen on open, applied with the grant.
5. **Dialogs and floating**: centered on the parent, capped at 80% of the
   monitor, following it; modal; a picture-in-picture rule; a hand-floated
   window's size remembered.
6. **Dimming** of every window but the focused one, with tide's
   exceptions.
7. **Input**: a tap-Super event; per-device settings and scroll factor.
8. **Outputs**: runtime rules (mode, scale, position, single-window
    width), reapplied on hotplug; the lid switch.
9. **Lock and keyboard**: a command that drops a dead lock, no pointer
    events while locked; on-demand layers keep the keyboard.
10. **Capture**: window identifiers over the IPC; capture start and stop
    events; tide's launcher and notifications hidden from capture.
11. **Outside clicks**: a focus-grab protocol or click event for the
    notification center.
12. **Sharp X11 at fractional scale**: unscaled Xwayland.
13. **Session**: the entry and wrapper tide starts it with.

Then tide's shell gets a tidewm backend beside its Hyprland one.
