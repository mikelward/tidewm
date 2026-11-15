# tidewm

tidewm is a Wayland compositor for tide, forked from [dwl], the compact,
hackable compositor based on [wlroots]. It carries tide's workspaces,
which span every monitor, sway's config syntax and sway's IPC. Like dwl,
it's configured by `config.h` when it's built; it also reads a config
file in sway's syntax at startup.

## Getting started

### Building tidewm
`main` builds against the latest release of [wlroots], 0.20, as
`config.mk` names it (`wlroots-0.20`). To build a release rather than
`main`, check out its tag ([tidewm's releases](#tidewms-releases)).

tidewm has the following dependencies:
- json-c (for tidewm's IPC)
- PCRE2 (for `for_window` rules' patterns)
- libinput
- wayland
- wlroots 0.20 (compiled with the libinput backend)
- xkbcommon
- wayland-protocols (compile-time only)
- pkg-config (compile-time only)

tidewm has the following additional dependencies if XWayland support is enabled:
- libxcb
- libxcb-wm
- wlroots (compiled with X11 support)
- Xwayland (runtime only)

Install these (and their `-devel` versions if your distro has separate
development packages) and run `make`. The first `make` copies
`config.def.h` to `config.h`, which is yours to edit.

To enable XWayland, you should uncomment its flags in `config.mk`.

`make test` also needs python3, wtype for the tests that press keys, and
wlr-randr for the tests that read and change outputs; built with
XWayland, its X11 tests need Xwayland. A test that lacks one of these
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
`$XDG_CONFIG_HOME/tidewm/config`, usually `~/.config/tidewm/config`, at
startup and again on `reload`, with no restart.
[`docs/config.md`](docs/config.md) says what it takes:

- key bindings and [modes](docs/config.md#modes) (`bindsym`, `bindcode`,
  `mode`), and [named bindings](docs/config.md#named-bindings) a shell
  can give other keys, or a key where it has none (`bindsym --name`,
  `bindsym --name NAME none`, `rebind`);
- switches, such as a laptop's lid (`bindswitch`);
- [the mouse](docs/config.md#the-mouse)'s drags and button bindings
  (`floating_modifier`, `bindsym --whole-window`, `resize_grab`);
- [window rules](docs/config.md#window-rules) (`for_window`);
- [outputs](docs/config.md#outputs), named by connector or by what they
  are, and placed beside each other as Hyprland's monitor rules place
  them (`output`);
- [input devices](docs/config.md#input) (`input`);
- layouts, a lone window's width and where a new window goes
  (`default_layout`, `ultrawide_aspect`, `layout_defaults`,
  `layout_cycle`, `single_window`, `new_window`);
- dimming inactive windows, fades, shadows and borders (`dim_inactive`,
  `fade_duration`, `shadows`, `shadow_blur_radius`, `default_border`,
  `client.focused` and the other classes);
- X11 at a fractional scale (`xwayland_scaling`) and
  [the focus guard](docs/config.md#the-focus-guard) (`focus_guard`);
- variables, and other files it includes, such as a shell's own settings
  (`set`, `include`).

`config.h`, read when tidewm is built, as in [dwm], holds the defaults the
file doesn't set, and the keys, buttons and rules in force without a file.
Once the file is loaded, its bindings replace `config.h`'s keys if it binds
any key at its top level, its rules replace `config.h`'s rules, and once it
says what the mouse does, its buttons replace `config.h`'s. sway's `exec`
and `exec_always` aren't settings: a command to start with tidewm goes in
`-s` ([Running tidewm](#running-tidewm)).

## Running tidewm

tidewm can be run on any of the backends supported by wlroots. This means you can
run it as a separate window inside either an X11 or Wayland session, as well as
directly from a VT console. Depending on your distro's setup, you may need to
add your user to the `video` and `input` groups before you can run tidewm on a
VT. If you are using `elogind` or `systemd-logind` you need to install polkit;
otherwise you need to add yourself in the `seat` group and enable/start the
seatd daemon.

When tidewm is run with no arguments, it will launch the server and begin
handling the config file's keys, or without one, the shortcuts configured
in `config.h`. There is no status bar or other
decoration initially; these are instead clients that can be run within the
Wayland session. Do note that the default background color is grey. This can be
modified in `config.h`.

If you would like to run a script or command automatically at startup, you can
specify the command using the `-s` option. This command will be executed as a
shell command using `/bin/sh -c`.  It serves a similar function to `.xinitrc`,
but differs in that the display server will not shut down when this process
terminates. Instead, tidewm will send this process a SIGTERM at shutdown and wait
for it to terminate (if it hasn't already). This makes it ideal for execing into
a user service manager like [s6], [anopa], [runit], [dinit], or [`systemd
--user`].

Note: The `-s` command is run as a *child process* of tidewm, which means that it
does not have the ability to affect the environment of tidewm or of any processes
that it spawns. If you need to set environment variables that affect the entire
tidewm session, these must be set prior to running tidewm. For example, Wayland
requires a valid `XDG_RUNTIME_DIR`, which is usually set up by a session manager
such as `elogind` or `systemd-logind`.  If your system doesn't do this
automatically, you will need to configure it prior to launching `tidewm`, e.g.:

    export XDG_RUNTIME_DIR=/tmp/xdg-runtime-$(id -u)
    mkdir -p $XDG_RUNTIME_DIR
    tidewm

### Status information

Information about selected layouts, current window title, app-id, and the
current workspace is written to the stdin of the `-s` command (see
the `STATUS INFORMATION` section in _tidewm_(1)).  This information can be used to
populate an external status bar with a script that parses the
information. Failing to read this information will cause tidewm to block, so if you
do want to run a startup command that does not consume the status information,
you can close standard input with the `<&-` shell redirection, for example:

    tidewm -s 'foot --server <&-'

If your startup command is a shell script, you can achieve the same inside the
script with the line

    exec <&-

dwl's [wiki] lists status bars that read this information.

### Focus guard

With `focus_guard enable` in the config file, or `focus_guard` in
`config.h`, a new window takes focus only when you asked for it, as tide's
SPEC §14 has it ([`docs/config.md`](docs/config.md#the-focus-guard)).

### The pointer

It starts in the middle of the leftmost output, where focus starts, not by
its top left corner.
Its theme and size are `XCURSOR_THEME`'s and `XCURSOR_SIZE`'s, as apps
read them, so it looks the same over windows and the desktop; without
them, the default theme at 24 pixels, and apps get that size.

### Focus when a window closes

Closing or minimizing the window you're in focuses the window under the
pointer, as tide's SPEC §6.7 and §14.2 and Hyprland's
`input:focus_on_close cursor` have it, going by where the windows left
are once they've moved into its space.
The pointer stays where it is. A layer surface over a window, such as a
notification, doesn't count: the window under it takes focus. Focus goes
to the window you were in before, as in dwl, when the pointer is over
nothing, such as the bar or empty space, or over a window the focus
guard holds, or when the window going is a dialog, which hands focus
back to its parent. A minimize also falls back when the pointer is on
another monitor, whatever it's over. A window you weren't in closing,
or being minimized, moves no focus.

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
area's width from an aspect of 2.1, and 60% from 3.2, as tide's SPEC §6.1
has it ([`docs/config.md`](docs/config.md), `single_window`).

### Dragging the master/stack edge

A left press within 4 pixels of the edge between the masters and the
stack, or on a shadow across it, drags it, as tide's SPEC §6.6 has it
([`docs/config.md`](docs/config.md#the-mouse)).

### Resizing a floating window by its shadow

A left press on a floating window's shadow drags the edge or corner it's
beside, as tide's SPEC §6.4 has it. With shadows off, the zone is the
8 pixels just outside the window
([`docs/config.md`](docs/config.md#the-mouse)).

### Dimming inactive windows

With `dim_inactive 0.07` in the config file, every window but the focused
one is dimmed, as tide's SPEC §6.2 has it
([`docs/config.md`](docs/config.md), `dim_inactive`).

### Fades and shadows

With `fade_duration 300` and `shadows active` in the config file, a window
fades in as it opens, and the focused one casts a soft shadow, as in
tide's `tidewm/tide.conf` ([`docs/config.md`](docs/config.md),
`fade_duration` and `shadows`).

### Border colors

A window's border has its class's color, set by sway's `client.focused`
and its other classes ([`docs/config.md`](docs/config.md), `client.*`).

### X11 at a fractional scale

With `xwayland_scaling disable` in the config file, X11 windows are in
their outputs' own pixels, so they're sharp at a fractional scale, as
tide's SPEC §17 asks ([`docs/config.md`](docs/config.md),
`xwayland_scaling`).

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

The IPC's `minimize` command hides a window, for tide's minimized windows
(its SPEC §6.7): it leaves the layout, and focus goes where it goes
when the window closes ([above](#focus-when-a-window-closes)), except
that a window under the pointer on another monitor doesn't take it.
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
following environment variable needs to be set before running the IDE or tidewm:

    export _JAVA_AWT_WM_NONREPARENTING=1

## Replacements for X applications

You can find a [list of useful resources on dwl's wiki].

## Background

tidewm began as [dwl], which is meant, like [dwm], to be:

- Easy to understand, hack on, and extend with patches
- One C source file configurable via `config.h`
- Tied to as few external dependencies as possible

dwl is not meant to provide every feature under the sun. Instead, like [dwm], it
sticks to features which are necessary, simple, and straightforward to implement
given the base on which it is built. tidewm adds what tide needs, in files
of its own (`ipc.h`, `workspace.h`, `settings.h` and the rest, included
from `dwl.c` as `client.h` is), so a merge from upstream dwl touches few
lines. The features it has from dwl are:

- Any features provided by [dwm]/Xlib: simple window borders, workspaces in
  place of tags, keybindings, client rules, mouse move/resize. Providing a
  built-in status bar is an exception to this goal, to avoid dependencies on
  font rendering and/or drawing libraries when an external bar could work well.
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

Feature *non-goals* for the main codebase include:

- Client-side decoration (any more than is necessary to tell the clients not to)
- Client-initiated window management, such as move, resize, and close, which can
  be done through the compositor
- Animations and visual effects, beyond tide's fades and shadows

## Acknowledgements

tidewm is dwl, with tide's additions. dwl began by extending the TinyWL
example provided (CC0) by the sway/wlroots
developers. This was made possible in many cases by looking at how sway
accomplished something, then trying to do the same in as suckless a way as
possible. dwl's patches are in [dwl-patches]; tidewm's `movestack` command
follows one of them.

Many thanks to suckless.org and the [dwm] developers and community for the
inspiration, and to the various contributors to dwl, including:

- **Devin J. Pohly for creating and nurturing the fledgling project**
- Alexander Courtis for the XWayland implementation
- Guido Cella for the layer-shell protocol implementation, patch maintenance,
  and for helping to keep the project running
- Stivvo for output management and fullscreen support, and patch maintenance

Also, thanks to dwl's current lead developers:

- @thanatos
- @guidocella

[dwl]: https://codeberg.org/dwl/dwl
[dwm]: https://dwm.suckless.org/
[`systemd --user`]: https://wiki.archlinux.org/title/Systemd/User
[anopa]: https://jjacky.com/anopa/
[dinit]: https://davmac.org/projects/dinit/
[dwl-patches]: https://codeberg.org/dwl/dwl-patches
[list of useful resources on dwl's wiki]: https://codeberg.org/dwl/dwl/wiki/Home#migrating-from-x
[runit]: http://smarden.org/runit/faq.html#userservices
[s6]: https://skarnet.org/software/s6/
[wlroots]: https://gitlab.freedesktop.org/wlroots/wlroots/
[wiki]: https://codeberg.org/dwl/dwl/wiki/Home#compatible-status-bars
