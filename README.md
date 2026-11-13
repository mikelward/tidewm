# dwl - dwm for Wayland

Join us on our [Discord server]  
Or Matrix: [#dwl-official:matrix.org]  
Or on our IRC channel: [#dwl on Libera Chat]

dwl is a compact, hackable compositor for [Wayland] based on [wlroots]. It is
intended to fill the same space in the Wayland world that [dwm] does in X11,
primarily in terms of functionality, and secondarily in terms of
philosophy. Like [dwm], dwl is:

- Easy to understand, hack on, and extend with patches
- One C source file configurable via `config.h`
- Tied to as few external dependencies as possible

## Getting Started:

### Latest semi-stable [release]
This is probably where you want to start. This builds against the [wlroots]
versions currently shipping in major distributions. If your
distribution's `wlroots` version is older, use an earlier dwl [release].
The `wlroots` version against which a given `dwl` release builds is specified
with each release on the [release] page

### Development branch [main]
Active development progresses on the `main` branch. The `main` branch is built
against the latest release of [wlroots]. PRs should target this branch unless they
depend on functionality that is not in the current release of `wlroots`.

### Preview branch [wlroots-next]
The `wlroots-next` branch is built against the git version of [wlroots], which
is unstable and changes frequently. PRs requiring functionality from the git
version of `wlroots` should target this branch.

### Building dwl
dwl has the following dependencies:
- json-c (for tidewm's IPC)
- PCRE2 (for `for_window` rules' patterns)
- libinput
- wayland
- wlroots (compiled with the libinput backend)
- xkbcommon
- wayland-protocols (compile-time only)
- pkg-config (compile-time only)

dwl has the following additional dependencies if XWayland support is enabled:
- libxcb
- libxcb-wm
- wlroots (compiled with X11 support)
- Xwayland (runtime only)

Install these (and their `-devel` versions if your distro has separate
development packages) and run `make`. If you wish to build against a released
version of wlroots (*you probably do*), use a [release] or a [0.x branch]. If
you want to use the unstable development `main` branch, you need to use the git
version of [wlroots].

To enable XWayland, you should uncomment its flags in `config.mk`.

`make test` also needs python3, and wtype for the tests that press keys; built
with XWayland, its X11 tests need Xwayland. A test that lacks one of these
skips, except in CI, where it fails.

`make install` puts the binary in `$(PREFIX)/bin` as `tidewm`, so it never
stands in for a distro's dwl, with its man page as `tidewm.1`. It installs
no session entry: tide's starts it, under uwsm. It does install
`tidewm-portals.conf`, which has xdg-desktop-portal use
xdg-desktop-portal-wlr for screen sharing and screenshots, and
xdg-desktop-portal-gtk for the rest, in a session whose
`XDG_CURRENT_DESKTOP` starts with `tidewm`.

### tidewm's releases
tidewm's releases are tags `v0.MINOR.PATCH` on `main`, the first `v0.0.1448`.
PATCH is the number of commits on `main` at the tag, so it only grows and no
two releases share one. MINOR goes up for a change that breaks a config file or
a client of the IPC, as semver's 0.x minor does. A release's tag is never moved,
so a pin on it always builds the same code. The release workflow tags every
push to main, as `make release-tag` prints; raise `RELEASE_MINOR` in
`config.mk` in the change that breaks a config or a client. `tidewm -v` names
the tag it was built from. Upstream dwl's own `v0.x` tags stay as they are.

## Configuration

tidewm reads a config file in sway's syntax, the one `-c FILE` names, or
`$XDG_CONFIG_HOME/tidewm/config`, usually `~/.config/tidewm/config`, at startup and again on `reload`, with no
restart: keys, the mouse, window rules, outputs, input devices, layouts
and the rest ([Config file](#config-file), and
[`docs/config.md`](docs/config.md) for what it takes). `config.h`, read
when tidewm is built, as in [dwm], holds the defaults the file doesn't
set, and the keys, buttons and rules in force without a file.

As in the [dwm] community, we encourage users to share patches they have
created. Check out the [dwl-patches] repository!

## Running dwl

dwl can be run on any of the backends supported by wlroots. This means you can
run it as a separate window inside either an X11 or Wayland session, as well as
directly from a VT console. Depending on your distro's setup, you may need to
add your user to the `video` and `input` groups before you can run dwl on a
VT. If you are using `elogind` or `systemd-logind` you need to install polkit;
otherwise you need to add yourself in the `seat` group and enable/start the
seatd daemon.

When dwl is run with no arguments, it will launch the server and begin handling
any shortcuts configured in `config.h`. There is no status bar or other
decoration initially; these are instead clients that can be run within the
Wayland session. Do note that the default background color is grey. This can be
modified in `config.h`.

If you would like to run a script or command automatically at startup, you can
specify the command using the `-s` option. This command will be executed as a
shell command using `/bin/sh -c`.  It serves a similar function to `.xinitrc`,
but differs in that the display server will not shut down when this process
terminates. Instead, dwl will send this process a SIGTERM at shutdown and wait
for it to terminate (if it hasn't already). This makes it ideal for execing into
a user service manager like [s6], [anopa], [runit], [dinit], or [`systemd
--user`].

Note: The `-s` command is run as a *child process* of dwl, which means that it
does not have the ability to affect the environment of dwl or of any processes
that it spawns. If you need to set environment variables that affect the entire
dwl session, these must be set prior to running dwl. For example, Wayland
requires a valid `XDG_RUNTIME_DIR`, which is usually set up by a session manager
such as `elogind` or `systemd-logind`.  If your system doesn't do this
automatically, you will need to configure it prior to launching `dwl`, e.g.:

    export XDG_RUNTIME_DIR=/tmp/xdg-runtime-$(id -u)
    mkdir -p $XDG_RUNTIME_DIR
    dwl

### Status information

Information about selected layouts, current window title, app-id, and the
current workspace is written to the stdin of the `-s` command (see
the `STATUS INFORMATION` section in `_dwl_(1)`).  This information can be used to
populate an external status bar with a script that parses the
information. Failing to read this information will cause dwl to block, so if you
do want to run a startup command that does not consume the status information,
you can close standard input with the `<&-` shell redirection, for example:

    dwl -s 'foot --server <&-'

If your startup command is a shell script, you can achieve the same inside the
script with the line

    exec <&-

To get a list of status bars that work with dwl consult our [wiki].

### Config file

tidewm also reads a config file in sway's syntax, from `-c FILE` or
`~/.config/tidewm/config`, and again on `reload`: key bindings and modes, named bindings a shell can give other keys,
the mouse's drags and button bindings, window rules, outputs, named by connector or by what they are and placed
beside each other as Hyprland's monitor rules place them, input devices,
and other files it includes, as a shell's own settings. Once it's loaded, its bindings replace
`config.h`'s keys and its rules `config.h`'s rules, and once it says what
the mouse does, `config.h`'s buttons.
[`docs/config.md`](docs/config.md) says what it takes.

### Focus guard

With `focus_guard enable` in the config file, or `focus_guard` in
`config.h`, a new window takes focus only when you asked for it: a grant
over the IPC names its app, or it's from the app you're in. The rest open
unfocused and urgent, as tide's SPEC §14 has it. An app's activation
marks its window urgent, unless a grant names it. A window opens on the
workspace it was launched from: one that opens after you've moved on is
there, unfocused, and that workspace turns urgent.
[`docs/config.md`](docs/config.md) says how.

### The pointer

It starts in the middle of the leftmost output, where focus starts, not by
its top left corner.
Its theme and size are `XCURSOR_THEME`'s and `XCURSOR_SIZE`'s, as apps
read them, so it looks the same over windows and the desktop; without
them, the default theme at 24 pixels, and apps get that size.

### Focus when a window closes

Closing the window you're in focuses the window under the pointer, as
tide's SPEC §14.2 and Hyprland's `input:focus_on_close cursor` have it,
going by where the windows left are once they've moved into its space.
The pointer stays where it is. A layer surface over a window, such as a
notification, doesn't count: the window under it takes focus. Focus goes
to the window you were in before, as in dwl, when the pointer is over
nothing, or over a window the focus guard holds, or when a dialog closes,
which hands focus back to its parent. A window you weren't in closing
moves no focus.

### Notifications and the keyboard

A layer surface with on-demand keyboard focus, such as a notification's
reply field, takes the keyboard when you click it, not when the pointer
crosses it. It keeps it until you click elsewhere, press a key binding or
run a command, or the launcher or the lock takes it, as tide's SPEC §14.1
has it. A window opening or closing, or the pointer crossing one, doesn't
take it.

A notification center's focus grab, over hyprland-focus-grab-v1, which
Quickshell's `HyprlandFocusGrab` speaks, closes it on a click outside,
as tide's SPEC §9 has it. While the grab holds, the keyboard stays with
the center and the pointer enters only it, so crossing away doesn't end
it. The click that ends it goes no further, as one outside a menu
doesn't. A key binding or command that moves focus, the launcher or the
lock end it too.

### A lone window

A tiled window alone on its monitor is centered at 80% of the window
area's width on an ultrawide, at least 2.1 times as wide as it's high,
and 60% from 3.2, in any layout, as tide's SPEC §1 has it. Below that it
has the whole width. The config file's `single_window` lines change the
widths and where they start.

Dragging a lone window's left or right side sets its width, which stays
centered, for that workspace on that monitor; `single_width reset` gives
the rules back.

The config file can also say which layout a workspace starts in on a
monitor, by its aspect, where ultrawide begins, the `mfact` and `nmaster`
each layout starts with, and where a new window goes, as tide's Layouts
page sets them ([`docs/config.md`](docs/config.md)).

### Dragging the master/stack edge

A left press on the edge between the masters and the stack, within 4
pixels either side, drags it, as tide's SPEC §6.6 has it. A resize drag, as Super and the
right button with `floating_modifier Mod4`, or `config.h`'s Alt, drags a
tile's nearest edge from anywhere on it. A press over a floating window, a layer surface
or a popup goes to it as usual, as does one on a monitor that isn't
focused or under a fullscreen or maximized window. The config file's
`resize_grab` sets how wide the grab is, and `0` turns it off
([`docs/config.md`](docs/config.md#the-mouse)).

### Dimming

With `dim_inactive 0.07` in the config file, or a `dim_inactive` above 0
in `config.h`, every window but the focused one is dimmed, by a
translucent black layer that clicks go through, as tide's SPEC §6.2 has
it. A monitor showing one window, or a fullscreen one, has nothing to
tell apart, so it doesn't dim. Nor do picture-in-picture, video, by the
app's content type (wp-content-type-v1), and a window the `dim disable`
command exempts, as a `for_window` rule can for a screen share's preview.
A window's menus and tooltips aren't dimmed.

### X11 at a fractional scale

With `xwayland_scaling disable` in the config file, X11 windows are in
their outputs' own pixels, as Hyprland's `xwayland:force_zero_scaling`
has them and tide's SPEC §17 asks: an X11 window is configured, and
shown, one of its pixels to one of the output's, so it's sharp at a
fractional scale. An app that scales itself, as by `GDK_SCALE`,
`QT_SCALE_FACTOR` or `Xft.dpi`, is then the size it would be scaled; one
that doesn't is small. Xwayland then sees each output at its own pixels,
placed by its place in the layout spread by the largest scale, so none
overlap, and its screen, RandR and clicks match. By default,
`xwayland_scaling enable`, X11 windows are scaled, as in sway, and blurry
at a fractional scale. A reload changes it at once.

### The lock

A screen locker that crashes leaves the screen locked, as
ext-session-lock requires and tide's SPEC §10 relies on: covered, with no
key or pointer event reaching a window. A new locker takes the lock over,
and from a TTY the IPC's `unlock` command drops it, keeping the session.
It can't end a lock a running locker holds.
[`docs/ipc.md`](docs/ipc.md) says how.

### Dialogs and floating windows

A window floats when it has a parent, is modal (X11's state or Wayland's
xdg-dialog-v1), is an X11 dialog, utility, toolbar or splash window, asks
for a fixed size, or has neither an app ID nor a title, or when a rule
floats it. One that floats as it opens goes in the middle of its parent,
or of its monitor's window area without one, as big as it asks but no
more than 80% of the monitor, as tide's SPEC §6.4 has it. A dialog stays
above its parent, and follows it to another workspace or output, to the
middle of it there. A browser's picture-in-picture window, titled
`Picture-in-Picture` or so, floats a quarter of the monitor each way in
the window area's bottom right corner, and is sticky, on every workspace.
A sticky window's dialogs stay with it, and a sticky dialog stays shown
when its parent moves.

Floating a window by hand, by a key or the `floating` command, puts it
where one of its app's floating windows last opened, moved or was resized
to, size and position, for as long as tidewm runs; the first time, it
stays where it was tiled. Dialogs, fixed-size windows and
picture-in-picture place themselves, so their places aren't kept for
their app.

### Minimized windows

The IPC's `minimize` command hides a window, as tide's SPEC §6.7 has it:
it leaves the layout, and focus goes to the window you were in before.
It stays on its workspace, which stays while it waits, and focusing it,
as tide's shell does from its tray, a grant or `focus`, brings it back.
Nothing else minimizes a window: an app's own request does nothing.

### Windows kept open

The IPC's `keep_open` command keeps a window open, for tide's pinned
windows (its SPEC §6.5). `kill`, from a key or the IPC, leaves it open,
and a `close_refused` window event lets a shell say why. A key bound
with `--kept-open` counts only while you're typing in such a window, so a
config can take an app's own close keys, as Ctrl+W, from it alone: every
other window still gets them. It's kept open on the workspace it's on:
on any other, however it got there, it isn't. Minimized, it's still
kept open.
The window's own close button still closes it.

### Touchpad gestures

A touchpad's pinches, swipes and holds go to the window under the
pointer, over pointer-gestures, as in sway, so Chrome zooms on a pinch.
tidewm binds none itself: sway's `bindgesture` isn't there.

### IPC

tidewm also answers sway's IPC on a socket it names in `SWAYSOCK` and
`I3SOCK`, so
`swaymsg`, Quickshell's I3 module, nwg-displays and other clients of
sway's socket work with it. [`docs/ipc.md`](docs/ipc.md) lists what it answers. Its
workspaces are on ext-workspace too, for Wayland clients that read that.
Its own `send_shortcut` command sends a window a shortcut, focused or
not, as tide's picker pastes with it. Its own `capture` event says when a screen share or screenshot starts
and stops, and of which window, for tide's Sharing pill, and its own
output event when focus moves to another output, for tide's shell.

### (Known) Java nonreparenting WM issue
Certain IDEs don't display correctly unless an environmental variable for Java AWT
indicates that the WM is nonreparenting.

For some Java AWT-based IDEs, such as Xilinx Vivado and Microchip MPLAB X, the
following environment variable needs to be set before running the IDE or dwl:

    export _JAVA_AWT_WM_NONREPARENTING=1

## Replacements for X applications

You can find a [list of useful resources on our wiki].

## Background

dwl is not meant to provide every feature under the sun. Instead, like [dwm], it
sticks to features which are necessary, simple, and straightforward to implement
given the base on which it is built. Implemented default features are:

- Any features provided by [dwm]/Xlib: simple window borders, tags, keybindings,
  client rules, mouse move/resize. Providing a built-in status bar is an
  exception to this goal, to avoid dependencies on font rendering and/or drawing
  libraries when an external bar could work well.
- Configurable multi-monitor layout support, including position and rotation
- Configurable HiDPI/multi-DPI support
- Idle-inhibit protocol which lets applications such as mpv disable idle
  monitoring
- Provide information to external status bars via stdout/stdin
- Urgency hints via xdg-activate protocol
- Support screen lockers via ext-session-lock-v1 protocol
- Various Wayland protocols
- XWayland support as provided by wlroots (can be enabled in `config.mk`)
- Layer shell popups (used by Waybar)
- Damage tracking provided by scenegraph API

Given the Wayland architecture, dwl has to implement features from [dwm] **and**
the xorg-server. Because of this, it is impossible to maintain the original
project goal of 2000 SLOC and have a reasonably complete compositor with
features comparable to [dwm]. However, this does not mean that the code will grow
indiscriminately. We will try to keep the code as small as possible.

Features under consideration (possibly as patches) are:

- Protocols made trivial by wlroots

Feature *non-goals* for the main codebase include:

- Client-side decoration (any more than is necessary to tell the clients not to)
- Client-initiated window management, such as move, resize, and close, which can
  be done through the compositor
- Animations and visual effects

## Acknowledgements

dwl began by extending the TinyWL example provided (CC0) by the sway/wlroots
developers. This was made possible in many cases by looking at how sway
accomplished something, then trying to do the same in as suckless a way as
possible.

Many thanks to suckless.org and the [dwm] developers and community for the
inspiration, and to the various contributors to the project, including:

- **Devin J. Pohly for creating and nurturing the fledgling project**
- Alexander Courtis for the XWayland implementation
- Guido Cella for the layer-shell protocol implementation, patch maintenance,
  and for helping to keep the project running
- Stivvo for output management and fullscreen support, and patch maintenance

Also, thanks to our current lead developers:

- @thanatos
- @guidocella

[wlroots]: https://gitlab.freedesktop.org/wlroots
[dwm]: https://dwm.suckless.org/
[`systemd --user`]: https://wiki.archlinux.org/title/Systemd/User
[#dwl on Libera Chat]: https://web.libera.chat/?channels=#dwl
[0.7-rc1]: https://codeberg.org/dwl/dwl/releases/tag/v0.7-rc1
[0.x branch]: https://codeberg.org/dwl/dwl/branches
[anopa]: https://jjacky.com/anopa/
[dinit]: https://davmac.org/projects/dinit/
[dwl-patches]: https://codeberg.org/dwl/dwl-patches
[list of useful resources on our wiki]: https://codeberg.org/dwl/dwl/wiki/Home#migrating-from-x
[main]: https://codeberg.org/dwl/dwl/src/branch/main
[wlroots-next]: https://codeberg.org/dwl/dwl/src/branch/wlroots-next
[release]: https://codeberg.org/dwl/dwl/releases
[runit]: http://smarden.org/runit/faq.html#userservices
[s6]: https://skarnet.org/software/s6/
[wlroots]: https://gitlab.freedesktop.org/wlroots/wlroots/
[wiki]: https://codeberg.org/dwl/dwl/wiki/Home#compatible-status-bars
[Discord server]: https://discord.gg/jJxZnrGPWN
[Wayland]: https://wayland.freedesktop.org/
[#dwl-official:matrix.org]: https://matrix.to/#/#dwl-official:matrix.org
