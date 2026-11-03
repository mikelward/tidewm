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
- **A config file** (#12): `bindsym` and `set` in sway's syntax, from `-c`
  or `~/.config/tidewm/config`, reread by `reload`, its errors in
  `GET_CONFIG` (`docs/config.md`).
- **Submaps** (#13): sway's `mode NAME { ... }` blocks and `mode`
  command, with the `mode` event, `GET_BINDING_MODES` and
  `GET_BINDING_STATE`.
- **Release and locked bindings** (#14): `--release` runs on a tap,
  `--locked` on the lock screen too.
- **Outputs** (#16): sway's `output NAME|* mode|scale|position|transform|
  enable|disable` lines and command, applied when an output appears and
  on reload.
- **Window rules** (#15): `for_window [criteria] commands`, matching
  `app_id`, `class`, `instance`, `title` and `shell` by regular
  expression, in place of `config.h`'s rules once a file is loaded.

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
- **A file's keys replace config.h's** (#12): once a file is loaded, its
  bindings are all the keys there are, as in sway, and its `for_window`
  lines all the rules (#15). The alternative was the file adding to and
  overriding `config.h`'s. Reversible: tide writes the whole file.
- **config.h's monitor rules stay** (#16): a file's output lines override
  them only where they say something, since the file can't yet set a
  monitor's layout, mfact or nmaster. Unlike keys and rules, which a file
  replaces whole. Reversible, once the layouts item gives the file those.
- **Rules run once** (#15): when a window appears, not again when its
  title changes, as sway's can. tide's rules match app IDs, which don't
  change; a title rule that needs it can come later.
- **A bad file at startup** (#12): tidewm says why on stderr and in
  `GET_CONFIG`, and starts with `config.h`'s keys, rather than refusing to
  start, so a typo can't lock anyone out. Reversible.

## Plan

1. **The rest of the runtime config**: per-device input and XKB.
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
7. **Input**: a tap-Super event, which `--release Super_L` nearly is, once
   a pointer button also ends the tap; per-device settings and scroll
   factor.
8. **Outputs**: a single-window width; the lid switch. Mode, scale,
    position and hotplug came with the runtime config (#16).
9. **Lock and keyboard**: a command that drops a dead lock, no pointer
    events while locked; on-demand layers keep the keyboard.
10. **Capture**: window identifiers over the IPC; capture start and stop
    events; tide's launcher and notifications hidden from capture.
11. **Outside clicks**: a focus-grab protocol or click event for the
    notification center.
12. **Sharp X11 at fractional scale**: unscaled Xwayland.
13. **Session**: the entry and wrapper tide starts it with.

Then tide's shell gets a tidewm backend beside its Hyprland one.
