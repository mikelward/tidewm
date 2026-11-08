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
- **Hiding the shell from capture waits** (maintainer, 2026-11-04): tide's
  launcher and notifications showing black to the far end of a share is
  deferred, below the Plan. It's large and has no sway model: an output
  source rendered from a second scene without them, which must follow
  every window, or wlr-screencopy dropped, which breaks its clients.

## Done

- **The mouse from the config** (#?): sway's `floating_modifier`, so
  Super and a drag move or resize a floating window, and its
  `bindsym --whole-window` button and scroll bindings, as Super and the
  middle button maximizing, as tide's SPEC §6.6 has them. `config.h`'s
  Alt drags stay until the file says what the mouse does.
- **Gestures** (#64): pointer-gestures, so a touchpad's pinch, swipe and
  hold reach the window under the pointer, as Chrome's pinch to zoom.
- **X11 at a fractional scale** (#45): with `xwayland_scaling disable`,
  X11 windows are in their outputs' own pixels, as with Hyprland's
  `xwayland:force_zero_scaling`, so they're sharp at a fractional scale,
  and Xwayland sees each output at its pixels, over an xdg-output of
  tidewm's own. The Plan's other half, starting Xwayland ahead of
  clients, wasn't needed: Xwayland takes no client until the window
  manager owns WM_S0, which wlroots takes after it watches the root.
- **Outside clicks** (#44): hyprland-focus-grab-v1, so Quickshell's
  `HyprlandFocusGrab` closes tide's notification center on a click
  outside it, but not on the pointer crossing away.
- **Capture events** (#41): the IPC's `capture` event says when an
  ext-image-copy-capture session starts and stops, and of which window,
  or of an output. Window identifiers were there already, as GET_TREE's
  `foreign_toplevel_identifier`, since the IPC came.
- **On-demand layers keep the keyboard** (#40): a notification's reply
  field takes the keyboard when clicked, and keeps it while a window opens
  or closes or the pointer crosses one, until a click, a key binding or a
  command moves it, or the launcher or the lock takes it.
- **A dead lock** (#39): the IPC's `unlock` drops a lock whose locker has
  gone, from a TTY, keeping the session; a new locker takes it over with
  no window shown or focused between. No pointer event reached a window
  behind a dead lock already, as dwl looks only at the lock's layer while
  locked; tests now say so.
- **The lid switch** (#38): sway's `bindswitch` runs a command when the
  lid closes or opens, as `output eDP-1 disable` and `enable`.
- **A lone window's width** (#37): a lone tiled window is centered at
  80% on an ultrawide, 60% from 3.2, by `single_window` rules.
- **Layout switches** (#36): the `input` event's `xkb_layout` says when a
  keyboard's active layout switches, and sway's `xkb_switch_layout`
  switches it.
- **Scroll factor** (#35): sway's `scroll_factor` input setting scales a
  pointer's scrolls, a client's virtual pointer's too.
- **Tap Super** (#34): a `--release` binding is a tap only with no pointer
  button or scroll meanwhile, and sway's `binding` event says when one
  runs, for tide's launcher.
- **Dimming** (#33): every window but the focused one is dimmed, at
  `dim_inactive`'s strength, except where there's one window to see,
  picture-in-picture, video, and what `dim disable` exempts.
- **A floating window's place, per app** (#31): floating a window by hand
  puts it where its app's last floating window was, for the session.
- **Dialogs follow their parent** (#29): a dialog stays above its parent
  and goes with it to another workspace or output.
- **Picture-in-picture** (#30): a window titled `Picture-in-Picture`, or
  so, floats sticky in the bottom right corner; sway's `sticky` command.
- **Dialog placement** (#28): a window that floats as it opens goes in
  the middle of its parent, or of the window area, no bigger than 80% of
  the monitor; modal windows, by xdg-dialog-v1, and nameless ones float.
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
- **A list of the bindings** (#56): tidewm's own `GET_BINDINGS`, the
  config file's `bindsym` and `bindswitch` lines as written, for tide's
  keyboard shortcuts page.
- **Outputs** (#16, #54): sway's `output NAME|* mode|scale|position|transform|
  enable|disable` lines and command, applied when an output appears and
  on reload. An output is named by its connector or by what it is,
  sway's identifier or Hyprland's description, and `position auto`,
  `auto-left`, `auto-up` and `auto-down` place it beside the others as
  Hyprland places monitors.
- **Window rules** (#15): `for_window [criteria] commands`, matching
  `app_id`, `class`, `instance`, `title` and `shell` by regular
  expression, in place of `config.h`'s rules once a file is loaded.
- **Layouts** (#19, #20, #21, #55): new windows join the end of the stack;
  three-column and two columns + stack beside tile and monocle, with
  tide's geometry; a layout, mfact and nmaster per workspace part, from
  its monitor's aspect; the `layout` event. The config file's
  `default_layout`, `ultrawide_aspect`, `layout_defaults` and `new_window`
  lines set them as tide's Layouts page does.
- **Fullscreen on open** (#27): a window the focus guard holds goes
  fullscreen once you go to it, and leaves a fullscreen window you're in
  as it is until then; focus ends a fullscreen that would hide it; an X11
  window fullscreen as it maps opens fullscreen.
- **Maximize** (#26): tidewm's own `maximize` command, `maximized` field
  and `maximize` event, and apps' own requests: one window over its
  monitor's window area, above the other tiles, until it's turned off,
  closes, or another window on its workspace takes focus.
- **The last input** (#25): sway's `GET_SEATS` with tidewm's own
  `last_input`, the last key or click, the window it went to and how long
  ago, for the shell's polkit agent.
- **Urgency marks** (#24): `urgent_order` in the tree, most recent mark
  highest, and sway's `urgent enable|disable` for the shell's own marks.
- **Notification grants** (#23): `grant --or-recent`, which lapsing
  unused brings up the app's most recent window, and `focus recent`,
  which does at once.
- **The focus guard** (#22): `focus_guard enable`, and the `grant` and
  `grant extend` commands. A new window takes focus only through a
  grant, from the app you're in, as a portal's dialog, or as a polkit
  prompt after a key press; the rest open held and urgent. A key press,
  a click or a focus move cancels a grant, and it lapses. Activation,
  Wayland's and X11's, is marked unless a grant names the app.
- **Input devices** (#17, #57): sway's `input NAME SETTING VALUE` lines
  and `input NAME { ... }` blocks, for `*`, a type, or a device's
  identifier or the name Hyprland gives it: XKB and repeat for keyboards,
  grouped as sway's smart grouping does, and libinput's settings for
  pointers; the `input` command, `GET_INPUTS` and the `input` event.

## Decisions needing review

- **config.h's buttons stay until the file speaks of the mouse** (#?):
  a file with no `floating_modifier` line and no button binding keeps
  `config.h`'s Alt drags and Alt+middle; with either, they're left out
  and, without a `floating_modifier` line, nothing drags, as in sway,
  whose default is none. The alternative was the file's mouse adding to
  `config.h`'s, unlike its keys, which replace them. Reversible.
- **A button's binding needs `--whole-window`** (#?): sway binds a title
  bar's click without it, and tidewm has no title bars, so a bare
  `bindsym Mod4+button2` is an error rather than a binding that never
  runs. Button bindings take no `--release`, `--locked` or
  `--kept-open`, which sway's take, as nothing needs them yet; one runs
  on the window the click focused, never over a layer surface. Reversible.
- **Dragging a tile floats it** (#?), as dwl's Alt drag does, where
  sway's `floating_modifier` moves a tile among the others and resizes it
  in place. tide's SPEC §6.6 drags only floating windows. Reversible.
- **Gestures go to apps, and none are bound** (#64): tidewm passes a
  touchpad's pinches, swipes and holds to the window under the pointer,
  as sway does and as dwl-patches' pointer-gestures patch does, but has
  no `bindgesture`, sway's gesture bindings: tide binds no gesture on
  Hyprland either, and sway's take a parser, `GET_BINDINGS` entries and
  its own tracking of a gesture's direction and distance. A gesture's
  start ends a tap of Super, as a scroll does. Reversible: `bindgesture`
  can come when something wants a three-finger swipe between workspaces.
- **A gesture command for the tests** (#64): tidewm's own `gesture
  swipe|pinch|hold FINGERS` makes a gesture on the cursor's events, as
  `switch` makes a lid move, since the headless backend has no touchpad
  and wlr-virtual-pointer no gestures. The alternative was a test that
  only sees pointer-gestures advertised, which says nothing of a gesture
  reaching a window. Reversible: only the tests use it.
- **Installed as `tidewm`, with no session entry** (#47): `make install`
  renames the binary and its man page, so a distro's dwl keeps its name,
  and leaves out upstream's `dwl.desktop`, since tide's entry starts
  tidewm under uwsm and a bare session has no bar. The build still makes
  `dwl`, which keeps upstream's Makefile rules. Reversible.
- **The portals go by the session's first name** (#47):
  `tidewm-portals.conf` picks wlr, then gtk, for an `XDG_CURRENT_DESKTOP`
  starting with `tidewm`, as sway's `sway-portals.conf` does for sway.
  tide's tidewm session puts `tidewm` before `tide` for it, since
  xdg-desktop-portal reads only the first name's file, and `tide`'s
  picks Hyprland's portal. Reversible.
- **X11 at a fractional scale is a setting, scaled by default** (#45):
  `xwayland_scaling disable` turns the scaling off, as tide's config
  will, and the default scales, as sway and dwl do, since an app that
  doesn't scale itself is small unscaled. The name is in sway's config
  style rather than Hyprland's `force_zero_scaling`. Reversible.
- **X11's space spreads the layout by the largest scale** (#45): an
  output's box there is its place in the layout times the largest scale,
  at its own pixels, so none overlap and they keep their arrangement,
  where Hyprland lays them in a row. Between outputs of different scales
  that leaves gaps, which nothing is in. Reversible.
- **Outside clicks are Hyprland's focus grab** (#44): tidewm takes
  hyprland-focus-grab-v1, vendored from hyprland-protocols under its
  BSD-3-Clause license, so tide's Quickshell center works as it does on
  Hyprland, rather than an IPC click event of tidewm's own. The click
  that ends a grab goes no further, as one outside a menu doesn't; the
  alternative was to pass it on to the window clicked. A key binding or
  command that moves focus, the launcher or the lock end the grab too,
  where a window opening is held back. Reversible.
- **What sway has goes after tide's own** (autopilot, 2026-11-04): the
  Plan takes tide's items first, as "finish the fork's features first"
  asked, then what sway has and tidewm lacks, the ones a laptop meets
  most first: gestures, shortcut inhibit, xdg-foreign and security
  context. Full desktops' portals (global shortcuts, remote desktop,
  input capture), accessibility, auto-rotation, and titlebars,
  animations and blur stay out: wlroots compositors lack most of them,
  and dwl leaves the last three out on purpose. Reversible.
- **The capture event is tidewm's own** (#41), as sway has none:
  `capture`, numbered after `layout`, where Hyprland's is `screencast`.
  It names the captured window by `con_id`, but not an output, which
  wlroots keeps to itself; naming it would mean tidewm's own output
  sources, as hiding the shell from capture will need anyway. A
  screenshot's session sends it too, start and stop close together.
  Reversible.
- **Hovering no longer focuses an on-demand layer** (#40): dwl gave one
  the keyboard when the pointer crossed it, under focus-follows-mouse;
  now only a click does, as tide's SPEC §14.1 has a notification's reply
  field, and as wlr-layer-shell allows ("requiring a click even if focus
  follows the mouse"). Otherwise moving the pointer over a notification
  would take your typing and keep it. Reversible.
- **The command is `unlock`** (#39), tidewm's own, as sway has none:
  Hyprland's is `hl.clear_crashed_lockscreen()`. It fails, doing nothing,
  while a locker holds the lock, so it never unlocks a working lock
  screen, and any client of the IPC socket, which is the user's own, may
  run it, as anyone at a TTY may restart the locker. Reversible.
- **The X11 map stall waits** (autopilot, 2026-11-04): Known bugs' X11
  window that sometimes never maps in the tests, about one run in 130 of
  a pair of X11 tests, on `main` too, is left there while the Plan goes
  on. Its cause is in wlroots and Xwayland rather than tidewm; the
  alternative was to chase it there first. Reversible: it's the next thing
  to pick up.
- **The lid is sway's bindswitch, and the config decides** (#38): a
  `bindswitch lid:on output eDP-1 disable` turns the panel off whether or
  not another output is on, as in sway, where tide's Hyprland handler acts
  only with an external display and leaves the rest to logind. The
  alternatives: the output command refusing to turn off the last output,
  or a lid action of tidewm's own. Its tests move a switch device of
  tidewm's own, by the new `switch lid|tablet on|off` command, as the
  headless backend has none and wlroots no virtual one. Reversible.
- **A lone window's width is by aspect, not by output** (#37), as tide's
  Layouts page sets it, from `config.h`'s tide rules, not dwl's whole
  width: tidewm's layouts already follow tide's (three-column on an
  ultrawide). The Plan said "a single-window width" under Outputs; a
  per-output width can come with the output lines. Reversible.
- **A scroll ends a tap too** (#34), as a pointer button does, though
  tide's SPEC §8.2 names only `Super+T`, `Super`+drag and `Super`+click:
  Super with a scroll is Super as a modifier. Reversible.
- **Dimming is off unless the config says** (#33): `config.h`'s
  `dim_inactive` is 0, as its `focus_guard` is off, and tide's config
  sets 0.07. The alternative was 0.07 in `config.h`. Reversible.
- **Only the focused window is undimmed** (#33): the one GET_TREE calls
  `focused`. While a layer surface, such as tide's launcher, has the
  keyboard, every window dims, unless its monitor shows one. The
  alternative: the last window focused stays undimmed then. Reversible.
- **A fullscreen window counts as the only one shown** (#33), so it never
  dims, like a lone window, as the windows under it can't be seen.
  Reversible.
- **The exemption is a command, `dim enable|disable|toggle`** (#33),
  tidewm's own, for a `for_window` rule, as Hyprland's `no_dim` rule is;
  its menus and tooltips are never dimmed. Reversible: sway has nothing
  to follow here.
- **A floating place is the app's, and only floating by hand takes it**
  (#31): tidewm keeps the place of each app's last floating window, size
  and position, other than a dialog's, a fixed-size window's or
  picture-in-picture's, which place themselves. Floating a window of that
  app by a key or the `floating` command puts it there; the first time,
  it stays where it was tiled, as in dwl. tide's SPEC §6.4 says floating
  windows remember their place per app for the session, and tide's
  `docs/compositor.md` that a hand-floated window gets its last floating
  size back. A window that floats as it opens, by a rule or its type,
  still goes in the middle (#28). The alternatives: a place per window,
  not per app, or a rule-floated window opening at its app's place too.
  Reversible.
- **Picture-in-picture is built in** (#30): by its title, as tide's
  Hyprland rule matches it, `^Picture[- ]in[- ][Pp]icture$`, at that
  rule's size and corner, rather than a `for_window` rule in tide's config,
  which would need sway's `resize set` and `move position` commands.
  Reversible: those commands can come, and the rule move to the config.
- **Every window that floats as it opens is placed** (#28): dialogs, ones
  a rule floats, and X11 ones too, which may have asked for a place of
  their own; the cap is 80% of the monitor, and the place stays inside its
  window area. Reversible.
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
- **A part's first layout** (#21, #55): `threecol` from an aspect of 2.1,
  else the monitor rule's, unless the config file's `default_layout` and
  `ultrawide_aspect` lines say otherwise. Reversible.
- **A reload resets mfact and nmaster only when their defaults change**
  (#55): on Hyprland, tide's layouts reload on their own, and every
  workspace's layouts start from the defaults again; tidewm has one
  reload for every setting, so one for another page leaves a split
  you've changed as it is. Reversible.
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
- **No notice without a shell** (#25): with no shell running, a window
  the guard holds shows only its urgent border and its workspace's
  urgency, where tide's Lua guard showed a Hyprland notification. tidewm
  draws no text, and the shell is the notification daemon, so there's no
  one to send a notification to. Reversible, if a fallback is wanted.
- **Focus ends a fullscreen that would hide it** (#27): a window taking
  focus, by a command, a notification or going to a held window, ends
  another's fullscreen on its workspace there, as it ends a maximize,
  unless it's that one's dialog. dwl left the fullscreen window covering
  it, so the keyboard went to a window you couldn't see. Reversible.
- **A held window's fullscreen waits** (#27): any request to make a
  window the guard holds fullscreen, its app's or a command's or rule's,
  waits until it has focus, so it never covers the window you're in. The
  alternative was to refuse it, leaving the app to ask again. Reversible.
- **Maximize is tidewm's own** (#26): sway has no maximize, so the
  `maximize [enable|disable|toggle]` command, the `maximized` field and
  the `maximize` window event are tidewm's, named as `fullscreen`'s are.
  Reversible before tide's shell uses them.
- **What ends a maximize** (#26): another window on its workspace taking
  focus on the same monitor, as tide's SPEC §6.3 says, but not a dialog of
  it, such as its file chooser, which would otherwise end it every time
  it opened. A window on another monitor, or another workspace, leaves it.
  Reversible.
- **Apps may maximize themselves** (#26): an xdg app's own maximize
  request, from its title bar's button, is honored, where dwl ignored it;
  X11 apps' requests still aren't heard. A floating window gets its place
  back after; one that opened maximized gets the place it would have
  opened in. Reversible.
- **The last input is GET_SEATS's** (#25): tidewm's own `last_input` on
  sway's seat, rather than a request of its own. Its window is the one the
  input reached, none for a key or click a binding took, since the app
  never saw it. Reversible before tide's agent reads it.
- **The marks' order is a field** (#24): tidewm's own `urgent_order`
  on each window, a sequence, rather than a list of marked windows, so
  the tree stays sway's shape. A mark on a window already marked makes it
  the most recent, as tide's guard re-announces it. Reversible before
  tide's shell reads it.
- **Notification grants' names** (#23): `grant --or-recent` and `focus
  recent APP`, tidewm's own for tide's `grant_or_recent` and
  `focus_recent`. sway's `[app_id=...] focus` focuses every match rather
  than the most recent. Reversible before tide's shell uses them.
- **X11 activation is marked, guard or not** (#22): `_NET_ACTIVE_WINDOW`
  marks the window urgent, as xdg-activation already did, and as sway's
  default `focus_on_window_activation urgent` does, where dwl activated
  the X11 surface without focusing it.

## Known bugs

- **An X11 app's own cursor is scaled** with `xwayland_scaling disable`:
  Xwayland's cursor images are in X11's pixels, and the scene shows them
  at their size in the layout, so at a fractional scale they're a little
  big and blurry. X11 drag icons are the same.
- **A race in wlroots loses an X11 window's state set before it maps**:
  wlroots' window manager watches a window's properties only once it has
  handled the window's creation, and mapping the window writes its own
  idea of `_NET_WM_STATE` over the window's. So an X11 app that says it's
  fullscreen right after making its window may open as it would otherwise.
  An EWMH message, rather than the property, isn't lost. A fix belongs in
  wlroots: read the property at the map request.
- **An X11 window sometimes never maps in the tests**: about one run in
  130 of a pair of X11 tests, on `main` too. The X client maps its window
  and Xwayland makes its surface, but never commits a buffer to it, so
  tidewm never sees it map. Part of it is wlroots': pairing a window with
  its surface reads properties synchronously, which leaves X events that
  arrive meanwhile in xcb's queue with nothing to wake its window manager
  for them. Waking it there gets the window paired, but the commit still
  doesn't come. Not found yet. A window mapped right after another, as an
  X11 app's dialog right after its window, stalls far more often: four
  runs in ten, which keeps such a test out of the suite for now.
- **A leak in dwl's output management**: LeakSanitizer finds 136 bytes
  from `wlr_output_swapchain_manager_prepare()` in
  `outputmgrapplyortest()` after wlr-randr applies a configuration. It's
  dwl's own code.

## Plan

1. **Shortcut inhibit**: keyboard-shortcuts-inhibit, so a VM or remote
   desktop can take Super and Alt+Tab.
2. **xdg-foreign**: a portal's dialog, as a file chooser, tied to the
   window that opened it.
3. **Security context**: security-context, so Flatpak apps don't get the
   privileged protocols, such as screencopy and the virtual keyboard.
4. **The rest of sway's**: touchscreens, drawing tablets,
   wlr-foreign-toplevel-management for taskbars and docks, adaptive sync
   and tearing control, and a scratchpad.

Then tide's shell gets a tidewm backend beside its Hyprland one.

Deferred: tide's launcher and notifications hidden from capture (Decided,
above).
