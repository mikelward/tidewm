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
- **Layouts** (#19, #20, #21): new windows join the end of the stack;
  three-column and two columns + stack beside tile and monocle, with
  tide's geometry; a layout, mfact and nmaster per workspace part, from
  its monitor's aspect; the `layout` event.
- **The focus guard** (#22): `focus_guard enable`, and the `grant` and
  `grant extend` commands. A new window takes focus only through a
  grant, from the app you're in, as a portal's dialog, or as a polkit
  prompt after a key press; the rest open held and urgent. A key press,
  a click or a focus move cancels a grant, and it lapses. Activation,
  Wayland's and X11's, is marked unless a grant names the app.
- **Input devices** (#17): sway's `input NAME SETTING VALUE` lines and
  `input NAME { ... }` blocks, for `*`, a type or a device's identifier:
  XKB and repeat for keyboards, grouped as sway's smart grouping does,
  and libinput's settings for pointers; the `input` command, `GET_INPUTS`
  and the `input` event.

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
- **Layout commands' names** (#11, #20): sway has no master layouts, so
  `nmaster`, `mfact` and `zoom` take dwl's names, and `layout` takes
  tide's modes (`tile`, `threecol`, `twocol`, `monocle`) plus dwl's
  `floating`. Reversible before tide's shell uses them.
- **Each layout's own mfact and nmaster** (#20, #21): `threecol` starts at
  0.5 with one master and `twocol` at 0.72 with two, as tide's do; the
  rest start from `config.h`'s monitor rule. Each workspace's part on a
  monitor keeps each layout's, so switching back finds it as it was.
  Reversible.
- **A part's first layout** (#21): `threecol` from an aspect of 2.1, else
  the monitor rule's, built in. tide's SPEC makes the modes and where
  ultrawide begins settings; they can come to the config file with the
  layouts' `mfact` and `nmaster`. Reversible.
- **The layout event** (#21): tidewm's own event, `0x8000001e`, as sway
  has none, giving the part's layout by tide's name and its symbol; and a
  `layout` field in `GET_WORKSPACES` and the tree. Reversible before
  tide's shell reads them.
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
- **config.h's input settings stay** (#17): a device takes the file's
  input lines over `config.h`'s XKB names, repeat and libinput values,
  as outputs take theirs over its monitor rules. sway's defaults are XKB's
  and libinput's own, which `config.h`'s mostly are. Reversible.
- **Virtual keyboards keep their keymap** (#17): input lines don't change
  `wtype`'s or an on-screen keyboard's, as in sway, since the client
  types in the keymap it sent.
- **A bad file at startup** (#12): tidewm says why on stderr and in
  `GET_CONFIG`, and starts with `config.h`'s keys, rather than refusing to
  start, so a typo can't lock anyone out. Reversible.
- **The guard is off by default** (#22): `config.h`'s `focus_guard` is 0,
  and tide's file turns it on with `focus_guard enable`, so dwl's
  behavior stands without it. Reversible.
- **The guard's commands are tidewm's own** (#22): `grant` and `grant
  extend` over the IPC, and the file's `focus_guard` lines, since sway
  has no focus guard, and its `no_focus` has no grants. Reversible
  before `tide launch` uses them.
- **exec grants nothing** (#22): a binding grants its app first
  (`grant foot; exec foot`), as `tide launch` does. `exec` could grant
  its own process instead. Reversible.
- **A held window waits for you** (#22): it's never handed focus by
  another window closing, an output change or a window mapping, only by
  going to it: the pointer, a click, `focus next`, its workspace, or
  `[con_id=N] focus`. Hyprland would focus the window under the pointer
  when one closes. Reversible.
- **Marks are the urgent flag** (#22): a window the guard holds is
  `urgent` in the tree and in events, so a restarted shell reads the
  marks there, with no `tide-attention` event or `announce_waiting`.
  Reversible.
- **X11 activation is marked, guard or not** (#22): `_NET_ACTIVE_WINDOW`
  marks the window urgent, as xdg-activation already did, and as sway's
  default `focus_on_window_activation urgent` does, where dwl activated
  the X11 surface without focusing it.

## Known bugs

- **A leak in dwl's output management**: LeakSanitizer finds 136 bytes
  from `wlr_output_swapchain_manager_prepare()` in
  `outputmgrapplyortest()` after wlr-randr applies a configuration. It's
  dwl's own code.

## Plan

1. **The focus guard's rest**: the marks' order, most recent first, and
   the shell's own marks, from notifications; a notification's grant,
   falling back to the app's most recent window, and that window at once
   for one whose notification is gone; the last key or click and its
   window, for the shell's polkit agent; a notice when no shell runs. The
   guard, grants and activation came with #22.
2. **Window states**: maximize, ending when another window on the
   workspace takes focus; fullscreen on open, applied with the grant.
3. **Dialogs and floating**: centered on the parent, capped at 80% of the
   monitor, following it; modal; a picture-in-picture rule; a hand-floated
   window's size remembered.
4. **Dimming** of every window but the focused one, with tide's
   exceptions.
5. **Input**: a tap-Super event, which `--release Super_L` nearly is, once
   a pointer button also ends the tap; `scroll_factor`; the `input`
   event's `xkb_layout`, when the active layout switches, for a layout
   indicator. Per-device settings came with the runtime config (#17).
6. **Outputs**: a single-window width; the lid switch. Mode, scale,
   position and hotplug came with the runtime config (#16).
7. **Lock and keyboard**: a command that drops a dead lock, no pointer
   events while locked; on-demand layers keep the keyboard.
8. **Capture**: window identifiers over the IPC; capture start and stop
   events; tide's launcher and notifications hidden from capture.
9. **Outside clicks**: a focus-grab protocol or click event for the
    notification center.
10. **X11**: sharp at fractional scale, by unscaled Xwayland. The first X
    client starts Xwayland, and a window it maps before wlroots' window
    manager watches the root is never managed (the tests' client waits
    for it): start Xwayland ahead of clients, or take on windows already
    there.
11. **Session**: the entry and wrapper tide starts it with.

Then tide's shell gets a tidewm backend beside its Hyprland one.
