# tidewm's config file

tidewm reads a config file in sway's syntax at startup, and again when it
gets the `reload` command. A binding's action is a list of the IPC
commands in [`ipc.md`](ipc.md), so anything `swaymsg` can do, a key can.

## Where it is

The first of these that's given:

1. `-c FILE` on tidewm's command line;
2. `$XDG_CONFIG_HOME/tidewm/config`;
3. `~/.config/tidewm/config`.

Without the file, `config.h`'s keys, buttons and rules work, as in dwl.
A file `-c` names that isn't there at startup is an error, on stderr and
in `GET_CONFIG`'s `errors`, and tidewm starts the same way.
Once a file that binds a key at its top level is loaded, its bindings are
all the keys there are. A file that binds none there, as when every `bindsym`
line failed or they sit in an `include` that's gone, keeps `config.h`'s
keys, and tidewm says so at startup, so there are always keys to fix it
with. Its
`for_window` lines all the rules, and `config.h`'s are left out, so a file
wanting dwl's defaults says them itself. `config.h`'s buttons, Alt and a
drag, stay until the file says what the mouse does, by a
`floating_modifier` line or a button's binding ([The mouse](#the-mouse)).

## Lines

Each line is one setting. Blank lines, and lines starting with `#`, are
skipped. Space at either end of a line goes, except a space escaped with a
backslash, which an `exec`'s shell will want. Line continuations aren't
supported yet. In an `input` or `output` line, a name or value may be in
`"`, as in sway, and an XKB name may be empty, as `""`.

| Setting | Does |
|---|---|
| `set $NAME VALUE` | Makes `$NAME` stand for the rest of the line in every later line. A name is letters, digits and `_`. Where names overlap, as `$w` and `$wide`, the longest that matches wins, as in sway |
| `include PATH` | As sway's: reads each file `PATH` matches, a word or text in `"` that may be a pattern such as `tide-*.conf`, in order, as if its lines were here, so a shell can keep settings of its own in a file the config takes in. `~/` is your home, and a relative path is from the directory of the file the line is in. A file that isn't there, or a pattern nothing matches, includes nothing. Variables set before it hold in it, and those it sets hold after. Only at the top level: a mode or input block an included file opens closes in it. Includes nest 10 deep at most. `reload` reads them again, and an error in one names that file |
| `bindsym KEYS COMMANDS` | Pressing `KEYS` runs `COMMANDS`, IPC commands separated by `;`. An `exec` takes the rest of the line, `;` and all, so it comes last |
| `bindcode KEYS COMMANDS` | As sway's: `bindsym`, with the key as its keycode, as below |
| `rebind NAME KEYS...`, `rebind NAME none` | tidewm's own: gives the binding `bindsym --name NAME` named these keys in place of the ones it has, or none, so a shell's keyboard shortcuts can change one ([Named bindings](#named-bindings)). Only at the top level |
| `bindswitch [--locked] [--desc TEXT] SWITCH:STATE COMMANDS` | As sway's: a switch turning on, off or either (`lid:on`, `lid:off`, `lid:toggle`, and `tablet:` likewise) runs `COMMANDS`; on the lock screen, only a `--locked` one. Not in a mode. `bindswitch lid:on output eDP-1 disable` and `bindswitch --locked lid:off output eDP-1 enable` turn a laptop's panel off while its lid is closed, and its windows move over; like sway, it does so whether or not another output is on |
| `for_window [CRITERIA] COMMANDS` | Runs `COMMANDS` on each new window `CRITERIA` match, as below |
| `output NAME SETTINGS` | Sets up the output called `NAME`, or every output for `*`, as below |
| `input NAME SETTING VALUE` | Sets up the input devices `NAME` names, as below; `input NAME {` … `}` takes a setting a line |
| `mode NAME {` … `}` | A submap: the `bindsym` lines inside are in force while `NAME` is the mode, and the top level's aren't. `NAME` is a word, or text in `"`, without `;`, which ends a command; opening a mode again adds to it |
| `single_window MIN_ASPECT WIDTH` | tidewm's own, tide's SPEC §1: a lone tiled window, in any layout, is centered at `WIDTH` of the window area, from 0.1 to 1, when the area is at least `MIN_ASPECT` as wide as it's high, from 0 to 100. Of several lines, the one with the largest `MIN_ASPECT` the area reaches wins, and none means the whole width. The file's lines replace `config.h`'s `single_window`, which is 80% from 2.1 and 60% from 3.2, as tide's |
| `default_layout normal\|ultrawide LAYOUT` | tidewm's own, tide's SPEC §6.1: the layout a workspace's new part on a monitor starts in, `tile`, `threecol`, `twocol` or `monocle`, below `ultrawide_aspect` or from it. Without the lines, `threecol` from it and `config.h`'s monitor rule's below. A part already made keeps its layout |
| `ultrawide_aspect N` | tidewm's own: how wide a monitor must be for its height, from 0.1 to 100, for its new parts to start in `default_layout ultrawide`'s layout; 2.1 without it |
| `layout_defaults LAYOUT [mfact F] [nmaster N]` | tidewm's own: the `mfact`, from 0.1 to 0.9, and `nmaster`, up to 100, from 0 for `tile` and 1 for the others, that `tile`, `threecol` or `twocol` starts with. A later line for a layout adds to an earlier one. Without one, `threecol` starts at 0.5 with one master, `twocol` at 0.72 with two, and `tile` as `config.h`'s monitor rule has it. A `reload` that changes them starts every layout of every part from them again, each part keeping its layout, as a reload of tide's layouts does on Hyprland; one that doesn't leaves them as they are |
| `layout_cycle LAYOUT ...` | tidewm's own, tide's Layouts page's cycle: the layouts `layout next` and `prev` go through, in order and round, each of `tile`, `threecol` and `twocol` at most once. `tile threecol twocol` without it |
| `new_window end\|top\|next\|master` | tidewm's own: where a new tiled window goes, at the `end` of the stack, its `top`, just after the masters, `next` after the focused window, or as the `master`. `end` without it, so the master stays put |
| `dim_inactive STRENGTH` | tidewm's own, tide's SPEC §6.2: dims every window but the focused one, from 0, none, to 1, black; tide's is 0.07. Without the line, `config.h`'s `dim_inactive`, 0 as it comes. A monitor showing one window, or a fullscreen one, doesn't dim, and nor do picture-in-picture, video, by the app's content type, and a window `dim disable` exempts ([`ipc.md`](ipc.md)) |
| `xwayland_scaling enable\|disable` | tidewm's own, tide's SPEC §17: with `disable`, as Hyprland's `xwayland:force_zero_scaling`, X11 windows are in their outputs' own pixels, so they're sharp at a fractional scale, and an app that scales itself, as by `GDK_SCALE`, `QT_SCALE_FACTOR` or `Xft.dpi`, is the size it would be scaled, while one that doesn't is small. `enable`, as without the line, scales them, as sway does, so they're blurry at a fractional scale. A reload changes it at once |
| `default_border none\|pixel [N]` | As sway's, but for every window, tiled and floating, as tidewm has no title bars and no `default_floating_border`: `none`, or a border `N` pixels wide, from 0 to 100, `config.h`'s `borderpx`, 1, without `N` or without the line. Blue marks the focused window, red one asking for attention. A fullscreen window has none. A reload changes every window's at once; tide's is `none`, as dimming shows which window is focused |
| `resize_grab PIXELS` | tidewm's own, tide's `resize_grab`: how many pixels either side of the master/stack edge a plain left press drags it from, from 0 to 32; `0` turns the drag off. 4 without it, `config.h`'s `resize_grab` ([The mouse](#the-mouse)) |
| `floating_modifier MOD [normal\|inverse]`, `floating_modifier none` | As sway's: holding `MOD`, the left button drags a floating window and the right one resizes it, or the other way round with `inverse` ([The mouse](#the-mouse)) |
| `focus_guard enable\|disable` | tidewm's own: turns the focus guard on or off, as below |
| `focus_guard grant_seconds S`, `focus_guard prompt_seconds S` | How long a grant holds, 10 by default, and how soon after a key press a polkit prompt may take focus, 2 by default, in seconds, more than 0 and at most a day |

`KEYS` is modifiers and a key, joined by `+`, as `Mod4+Shift+Return`:

- **Modifiers:** `Shift`, `Control` or `Ctrl`, `Mod1` or `Alt`, `Mod4`,
  `Super` or `Logo`, `Mod2`, `Mod3`, `Mod5` and `Lock`. Caps Lock is
  ignored when matching, as in dwl, so `Lock` is too: `Lock+Mod1+x` is
  `Mod1+x`.
- **Keys:** a keysym name, such as `Return`, `space`, `period` or `F1`, as
  `xkbcommon` names them, in any case. A letter is the key itself: `X` is
  the x key, so `Mod1+X` and `Mod1+x` are the same binding. A key matches
  whether it's typed shifted or not, so `Mod1+Shift+period` and
  `Mod1+Shift+greater` both work on a US keyboard.
- **Keycodes:** a `bindcode` line's key is a keycode, as `wev` and `xev`
  show it, evdev's code plus 8, from 8 up: `bindcode Mod4+38 ...` is
  Super and the key where a US keyboard has A, whatever the layout. It
  takes the same flags as `bindsym`, but not `--whole-window` or
  `--name`. A `bindcode` binding of a key comes before a `bindsym` one,
  as in sway.

A later binding of the same keys replaces an earlier one, though a
`--release` binding is apart from the press one, and a `--kept-open` one
from one for every window. Holding a key repeats its
press binding, as dwl's keys do. Flags, before the keys:

- **`--release`:** runs when the key comes up, if no other key, pointer
  button, scroll or touchpad gesture came meanwhile: a tap. The modifiers
  are as they were before the key, so `bindsym --release Super_L ...` is
  Super tapped alone, and not `Super+T`, `Super`+click, `Super`+drag or
  `Super` with a scroll or a swipe. The window still gets the key, down and up, as in sway. The IPC's
  `binding` event says when a binding runs ([`ipc.md`](ipc.md)), so
  `bindsym --release Super_L nop launcher` is a tap a shell can listen for.
- **`--locked`:** runs on the lock screen too. Other bindings, and all of
  `config.h`'s keys, don't.
- **`--kept-open`:** tidewm's own: counts only while the keyboard is in a
  window kept open, or a popup of one (the IPC's `keep_open`,
  [`ipc.md`](ipc.md)), and then before a binding of the same keys for
  every window. Otherwise the key goes to the window, as if it weren't
  bound. `bindsym --kept-open Ctrl+w kill` takes Ctrl+W from a window
  kept open alone, which `kill` then leaves open, and every other window
  still gets it.

- **`--whole-window`:** as sway's: binds a mouse button, or a scroll,
  rather than a key, over a window or the desktop ([The
  mouse](#the-mouse)).
- **`--name NAME`:** tidewm's own: names the binding, so a `rebind`
  line can give it other keys ([Named bindings](#named-bindings)).
- **`--desc TEXT`:** tidewm's own, as Hyprland's `bindd`: says what the
  binding does, for a shell's list of keyboard shortcuts, as
  `bindsym --desc "Open a terminal" $mod+Return exec foot`. `TEXT` is a
  word, or text in `"`; `--desc=TEXT` works too. The IPC's
  `GET_BINDINGS` gives it ([`ipc.md`](ipc.md)). A `bindswitch` line
  takes it as well.

Any other flag is an error.

```
set $mod Mod4
bindsym $mod+Return exec foot
bindsym $mod+1 workspace 1
bindsym $mod+Shift+1 move container to workspace 1
bindsym $mod+Shift+r reload
bindsym Control+Mod1+F2 chvt 2
bindsym --release Super_L exec launcher
bindsym --locked XF86AudioMute exec pactl set-sink-mute @DEFAULT_SINK@ toggle
```

Ctrl+Alt+F1…F12 switch to that VT whatever the file binds, locked or not,
as in sway: a file that lost its keys still leaves a way out. A binding of
your own on one of them comes first.

## Named bindings

A shell's keyboard shortcuts, as tide's, can give a binding other keys
if the config names it. `bindsym --name NAME` names one, `NAME` being a
lower-case letter, then lower-case letters, digits and `_`, as
`--name terminal` or `--name=terminal`. A name is one action: several
lines can give it keys, in one mode, each running the same commands
with the same flags, and those are its default keys. A later line that
gives the name another command, flags or mode redefines it, as a later
binding of the same keys replaces an earlier one: the name's earlier keys
go, and its default keys start again from that line. So a config that
includes tide's and binds tide's names to its own commands wins. A
line's `--desc` is the name's, for all its keys; a redefined name has
only its new lines'.

`rebind NAME KEYS...` gives it these keys in place of the ones it has,
and `rebind NAME none` takes them all away. Like a `bindsym` line, a
key a rebind gives takes it from the binding that had it. A name no line
has given is written to stderr and skipped, as Hyprland's tide does, so
a shell's file that outlives a name the config dropped doesn't stop it
loading. The IPC's `GET_NAMED_BINDINGS` lists each name, with its
default keys and its keys now ([`ipc.md`](ipc.md)).

```
set $mod Mod4
bindsym --name terminal $mod+t exec foot
bindsym --name float $mod+Shift+f floating toggle
bindsym --name float $mod+Insert floating toggle
include tide-*.conf
```

with a shell's `tide-keys.conf` holding

```
rebind terminal Mod4+Return
rebind float none
```

has `Mod4+Return` open foot, and no key float a window.

## The mouse

As in sway, `floating_modifier Mod4` has Super and the left button drag a
floating window, and Super and the right one resize it by its bottom
right corner, which the pointer jumps to, as dwl's Alt does;
`floating_modifier Mod4 inverse` swaps the buttons, and
`floating_modifier none` turns both off. The modifier counts whenever
it's held, with others or not, as in sway; one of several, as
`Mod4+Shift`, counts when all of them are. A tiled window floats as it's dragged, as in dwl, where sway would
move it among the tiles. A tiled window's resize drags its master/stack
edge nearest the pointer instead, as below, and only one with no edge,
as in `monocle`, floats.

With no modifier, a left press within `resize_grab` pixels, 4 by default,
either side of the edge between the masters and the stack drags it, as
tide's SPEC §6.6 has it: the edge follows the pointer until the button
comes up, setting the layout's `mfact`, as the `mfact` command does. The
pointer shows a resize cursor over the edge, and the windows either side
see neither the press nor the drag.

- **The edges** are `tile`'s, `twocol`'s, and `threecol`'s with one
  stack window. With stacks either side of `threecol`'s master, either
  of its sides drags it, and it grows or shrinks both ways.
- **A lone window's** left and right sides drag its width, and it stays
  centered. The width is its workspace's own on that monitor, in place of
  the `single_window` lines, whenever that workspace is down to one
  window there, until `single_width reset` ([`ipc.md`](ipc.md)).
- **There's no edge** between a stack's rows, which are always equal,
  between `twocol`'s masters, which share their side equally, or in
  `monocle`.
- **The press goes to what's there**, as usual, over a floating window,
  a layer surface, such as a notification or a bar's popover, or a
  window's popup; on a monitor that isn't focused; under a fullscreen or
  maximized window; and while a menu grabs the pointer or a focus grab
  holds, so a click outside it closes it.

`bindsym --whole-window` binds a button, with modifiers or not, as
`bindsym --whole-window Mod4+button2 maximize toggle`. Its commands run
when the button is pressed over a window, which takes focus first, as a
click gives it, or over the desktop, but not over the bar or another
layer surface; the window, or app, never sees that press. Buttons are
sway's: `button1` to `button3` the left, middle and right, `button4` and
`button5` a scroll up and down, `button6` and `button7` left and right,
which run for each scroll event, so many times in a touchpad's scroll, and `button8` and `button9` the side
buttons, or evdev's names, `BTN_LEFT`, `BTN_MIDDLE`, `BTN_RIGHT`,
`BTN_SIDE`, `BTN_EXTRA`, `BTN_FORWARD`, `BTN_BACK` and `BTN_TASK`. A
button needs `--whole-window`, as tidewm has no title bars, where sway's
binds a title bar's click without it, and it takes no `--release`,
`--locked` or `--kept-open`. A mode's own button bindings count while
it's the mode, as its keys do. No button binding runs while the screen is
locked.

Without a `floating_modifier` line or a button's binding, the file leaves
`config.h`'s buttons in force: Alt and the left button move a window,
Alt and the right one resize it, and Alt and the middle one float or tile
it. With either, even a button's binding a `rebind` takes away, `config.h`'s buttons are left out, as its keys are, so
the file's are all there are, and without a `floating_modifier` line no
modifier drags, as in sway.

```
floating_modifier Mod4 normal
bindsym --whole-window Mod4+button2 maximize toggle
```

## Modes

As in sway, the `mode NAME` command makes a mode's bindings the ones in
force, and `mode default` goes back to the top level, so a mode's own
keys must include a way out. Modes don't nest, and `default` can't be
opened. A `mode` line that fails, as with a flag tidewm's doesn't take,
takes the lines to its `}` with it, so they're never read as the top
level's. An `input` or `output` block in a mode, which can't hold one,
goes the same way, to its own `}`, and the mode stays open after it. A
load, `reload` included, starts at the top level.

```
bindsym $mod+r mode "resize"
mode "resize" {
    bindsym h mfact -0.05
    bindsym l mfact +0.05
    bindsym Escape mode default
}
```

## Window rules

As in sway, `for_window [CRITERIA] COMMANDS` runs `COMMANDS` on each new
window that every criterion in `CRITERIA` matches, before its `new` event,
so that says where the rules put it. Each rule that matches runs, in the
file's order, and every window command, such as `floating`, `move` or
`zoom`, acts on the new window, not the focused one. A monitor's commands,
such as `layout` or `mfact`, act on the focused monitor, as ever. A rule
runs once, when the window appears; a title that changes later isn't
matched again.

`CRITERIA` is `key=value` pairs, as in `[app_id="^firefox$" title=Private]`.
A value is a word or text in `"`, where `\"` is a quote, and is a PCRE2
pattern, as sway's are, matching anywhere unless anchored. It's read as
UTF-8, with Unicode's classes, so `\w` takes `é` too, and `(?i)` ignores
case:

| Key | Matches |
|---|---|
| `app_id` | A Wayland window's app ID; an X11 window has none |
| `class`, `instance` | An X11 window's `WM_CLASS`; a Wayland window has neither |
| `title` | The title |
| `shell` | `xdg_shell` or `xwayland` |

```
for_window [app_id="^pavucontrol$"] floating enable
for_window [class="^Steam$"] move to workspace games
for_window [title="Picture-in-Picture"] floating enable; move to output right
```

`for_window` can't be in a mode.

## Outputs

As in sway, `output NAME SETTINGS` sets up the output called `NAME`, or
every output for `*`. `NAME` is its connector, as `GET_OUTPUTS` names it,
or what it is, so its settings follow it from one port to another: sway's
identifier, its make, model and serial joined by spaces, with `Unknown`
for any it doesn't give, or Hyprland's description, which leaves those
out and has no commas, as `"Dell Inc. DELL U2720Q 1234ABC"`. Quote a name
with spaces. An output's own lines override `*`'s, and one naming what it
is overrides one naming its connector, as in sway; a later line for a
name adds to an earlier one. `config.h`'s monitor rules still apply
first, for what the file doesn't say, such as the layout. As in sway, an
`output NAME {` block, closed by `}`, gives each line inside it to
`NAME`, so the file nwg-displays saves loads as it is.

| Setting | Does |
|---|---|
| `mode [--custom] WIDTHxHEIGHT[@HZ]` | The mode, `Hz` or not after the rate, as `2560x1440@144Hz`. It's one the output lists, the fastest at that size if no rate is given, or a rate of 0, or else a custom mode; with `--custom`, always a custom mode. `resolution` and `res` say the same |
| `scale S` | The scale, above 0 and up to 10 |
| `position X Y` | Where it is in the layout, in logical pixels; `pos` and `X,Y` work too |
| `position auto-right`, `auto-left`, `auto-up`, `auto-down` | Beside the others, on that side, as Hyprland's monitor rules place it; `auto` is `auto-right`, where an output without a position goes |
| `transform T` | `normal`, `90`, `180`, `270`, `flipped`, or `flipped-` one of those |
| `enable`, `disable` | Turns it on or off; a disabled output's windows move to the focused one. The file's lines never leave every output off, as below |
| `adaptive_sync on\|off` | Variable refresh rate, off without the line. An output that can't have it stays off, as in sway |
| `power on\|off` | In the `output` command, as sway's, turns an output that's on off, keeping its windows and place, as an idle daemon does, or on again. An output line takes it but leaves it, so every output starts on. `dpms` is its old name |
| `scale_filter linear\|nearest\|smart` | Taken, as sway's, but left: tidewm always scales linearly, as `GET_OUTPUTS` says |

The lines apply when an output appears, at startup or when it's plugged
in, and again on `reload`, when each output starts over from `config.h`'s
monitor rule and its preferred mode, so a setting taken out of the file
goes. An output that refuses its settings at startup, as a mode it can't
show, gets its preferred mode instead, and stderr says so. One that refuses
that too, as a GPU that can't drive it, gets the first of its other modes
it takes; stderr names the mode it's on, or says it stays dark. The `output`
command takes the same settings, and sets them at once, until a reload; it
fails for a name no output has. An output enabled again keeps the mode
it had and goes back to the position it was last given; a mode or
position given while it's disabled, by the command or its line, is used
once it's enabled. The file's lines never leave every output off, at
startup, on `reload` or when an output is unplugged, since a session
that's dark can't be fixed from inside it. If they disable every output
there is, as a file saved while docked does for a laptop started on its
own, one stays on: the first connected at startup, or one that's on at a
reload, which fails. If the last output that's on is unplugged, the first
connected of those the file's line disabled comes on; one a command
disabled since, as a lid's `bindswitch` does, stays off, and so does one
in power saving. It stays on when the other is plugged in again, until a
`reload`. stderr and `GET_CONFIG`'s `errors` name it. A command can turn every
output off: the `output` command's `disable`, from a key, a `bindswitch`
or the socket, and an output management client's request, as
`wlr-randr`'s, do as they're asked, whatever else is on. Enabling or disabling an output wakes it from the power
saving an idle daemon may have put it in. The command's `output NAME power
on|off|toggle`, alone, puts it in that power saving, or takes it out, as
sway's does ([`ipc.md`](ipc.md)), and fails for an output that's disabled.

An output without a position, or with an `auto` one, is placed as
Hyprland places monitors: in the order they were connected, each beside
the outputs given positions, those placed before it and the origin, at 0
across. So one `auto-left` of an output at 0, 0 is at minus its width.
They move when an output beside them changes size or goes. One that an
output management tool such as `wlr-randr` or kanshi moves stays where
it was put, until a reload.

```
output eDP-1 scale 1.5
output DP-1 mode 2560x1440@144Hz position 1707 0
output "Dell Inc. DELL U2720Q 1234ABC" position auto-up
output HDMI-A-1 disable
output "HDMI-A-2" {
    mode 1920x1080@60.0Hz
    pos 2560 0
    adaptive_sync on
}
```

## Input

As in sway, `input NAME SETTING VALUE` sets up the input devices `NAME`
names, and an `input NAME {` … `}` block takes a `SETTING VALUE` a line.
`NAME`, a word or text in `"`, is one of:

- a device's identifier, as `GET_INPUTS` gives it, such as
  `1:1:AT_Translated_Set_2_keyboard`: its vendor and product numbers,
  then its name, with `_` for each space;
- its name as Hyprland gives it, `GET_INPUTS`' `hyprland_name`, such as
  `at-translated-set-2-keyboard`: its name in lower case, with `-` for
  each space, newline or comma, or `unknown-device` for none, and `-1`,
  `-2` and so on after it for each device already there with the same, as
  tide's input settings name a device;
- `type:keyboard`, `type:pointer` or `type:touchpad`, a touchpad being a
  pointer that can tap;
- `*`, every device.

A device takes `*`'s settings, then its type's, then its own, each over
the last, and all over `config.h`'s, so a setting the file doesn't give
is `config.h`'s. A later line for a name adds to an earlier one.

| Setting | Does |
|---|---|
| `xkb_layout`, `xkb_variant`, `xkb_options`, `xkb_model`, `xkb_rules` | A keyboard's keymap, in XKB's names, as `xkb_layout us,de` or `xkb_options grp:alt_shift_toggle` |
| `repeat_rate N`, `repeat_delay MS` | A held key's repeats a second, and how long it's held before they start |
| `tap`, `drag`, `drag_lock` | A touchpad's tap to click, tap and drag, and drag lock: `enabled` or `disabled` |
| `tap_button_map` | Which buttons one, two and three fingers tap: `lrm` or `lmr` |
| `natural_scroll`, `dwt`, `left_handed`, `middle_emulation` | Natural scrolling, disable while typing, left-handed buttons, and the middle button from both: `enabled` or `disabled` |
| `scroll_method` | `none`, `two_finger`, `edge` or `on_button_down` |
| `click_method` | `none`, `button_areas` or `clickfinger` |
| `accel_profile`, `pointer_accel` | `adaptive` or `flat`, and the speed, from -1 to 1 |
| `scroll_factor` | How much a pointer's scroll counts, 0 or more, 1 as it comes, as in sway: tidewm's own, so a client's virtual pointer has it too |
| `events` | `enabled`, `disabled` or `disabled_on_external_mouse` |

```
input type:keyboard {
    xkb_layout us,de
    xkb_options grp:alt_shift_toggle
}
input type:touchpad {
    tap enabled
    natural_scroll enabled
}
input 1133:49291:Logitech_G502 pointer_accel -0.3
```

Keyboards with the same keymap and repeat share their modifiers, as sway's
default `smart` grouping has them, so Shift on one and a key on another
make a capital. The pointer settings are libinput's, so they act on
libinput's devices, where the device has them. A client's virtual
keyboard, such as `wtype`'s, keeps its own keymap, and a virtual pointer
has no libinput settings, but `GET_INPUTS` lists both.

The lines apply when a device appears, and again on `reload`, when each
device starts over from `config.h`'s settings, so a setting taken out of
the file goes. `xkb_` settings XKB can't make a keymap of are an error in
the file: what a keyboard gets from `*`'s line, then `type:keyboard`'s,
then its own, so a variant may take its layout from a broader line, but
not one that layout lacks. The `input` command takes one setting, as a
line does, and sets it at once, until a reload; it fails for a name no
device has, or settings XKB can't make a keymap of, leaving the keymap as
it was.

## The focus guard

tidewm's own, from tide's SPEC §14: nothing takes the keyboard unless you
asked for it. `config.h`'s `focus_guard` says whether it's on without the
file. With it on, a new window takes focus when:

- **A grant names its app**, from the IPC's `grant` command
  ([`ipc.md`](ipc.md)), as `tide launch`, a notification's click or a
  terminal's shell makes one. Or the grant names a process, and the
  window's descends from it.
- **It's from the app you're in:** the same app ID, or the same process.
- **It's a portal's dialog,** such as a file chooser, while a window has
  focus, since it isn't the app's own process. `config.h`'s
  `portal_classes` lists them.
- **It's a polkit agent's prompt,** within `prompt_seconds` of a key
  press. `config.h`'s `prompt_classes` lists them.

`exec` grants nothing, so a key binding that starts an app grants it
first, as `tide launch` does: `bindsym Mod4+Return grant foot; exec foot`.

Any other new window opens where it would, held from focus and marked
urgent. Going to it takes it out of that: the pointer, a click,
`focus next`, its workspace, or `[con_id=N] focus`. The pointer resting
on it as the window you're in closes doesn't: focus goes to the window
you were in before ([README](../README.md#focus-when-a-window-closes)).
A window with neither
an app ID nor a title, such as Chrome's own notification, is a status
popup: it's held too, but never marked, and only the pointer or a click
focuses it.

A held window leaves the window you're in as it is: one that's fullscreen
stays so until you go to the held window. A held window that asks for fullscreen, as a game or `mpv
--fullscreen` does as it opens, or later, opens as it would otherwise and
goes fullscreen once you go to it, unless it takes the request back
first. So does one a `fullscreen` command or rule makes fullscreen.

- **A grant is used once:** by the first window that matches, or by an
  activation of a window its app already had. One that names an app is
  used before a wildcard.
- **A grant is canceled** by a key press, a click (even in the window
  you're in), or focus moving to another window by any means, including a
  new window taking it with no grant, as one from the app you're in does,
  or any with the guard off. These aren't moving on: focus
  coming back to the window you were in, as when a launcher closes; focus
  a grant gave, even once a launcher that held the keyboard closes; and
  focus moving as an output goes.
- **A canceled grant still places its window**, as Hyprland's
  `initial_workspace_tracking` does (tide's SPEC §14.2): if you've left
  the workspace it was made on, its window opens there, or on its
  `--workspace`, unfocused and urgent, and that workspace turns urgent.
  So an app you launch on `docs` that opens after you move to `review`
  opens on `docs`. A workspace renamed since is still found; one gone
  since is made again by name. A wildcard grant without `--pid`, or a
  notification's, places nothing. A dialog goes with its parent.
- **A grant lapses** after `grant_seconds`. A notification's grant,
  `grant --or-recent`, then brings up the app's most recent window,
  unless a window or an activation used it or you moved on; with no
  window, tidewm says so on stderr.

An activation, xdg-activation's or X11's `_NET_ACTIVE_WINDOW`, marks the
window urgent rather than focusing it, unless a grant names its app, with
the guard on or off. With it off, a new window still uses its grant, and
goes to the grant's workspace, though every window takes focus but one a
canceled grant places on another workspace.

## Errors

Each error names the file, the line and what's wrong, as
`/path/to/config:12: no setting is called bindgesture`.

- **At startup**, tidewm writes the errors to stderr and leaves those
  lines out: the rest of the file loads, with its keys and the commands
  it starts, so a session still comes up to fix it in. A keymap XKB can't
  make would leave the keyboard typing nothing, so then every `xkb_`
  line is left out, and the keyboards type in the environment's layout
  (`XKB_DEFAULT_LAYOUT`), or XKB's own.
  If it's the environment's names (`XKB_DEFAULT_RULES`, `_MODEL`,
  `_LAYOUT`, `_VARIANT`, `_OPTIONS`) XKB can't make a keymap of, tidewm
  says so on stderr, naming them, and starts without them: keyboards
  have XKB's own layout, `us`, where `config.h` and the file name none.
- **On `reload`**, the command fails with the first three errors.
- **Either way**, `GET_CONFIG` lists them all, until a load works.

An output that refuses its settings, as a mode it can't show, is an error
too, though not one in the file, so the rest of the file still loads. On
`reload`, the output keeps what it had, and the command fails, naming it;
at startup, or when it's plugged in, it gets its preferred mode, or
another it takes. Either
way, `GET_CONFIG` lists it.

A binding's commands are only checked when the key is pressed. One that
fails is written to stderr, with the binding's keys.

## Over the IPC

| Request | Gives |
|---|---|
| `reload` (a command) | Reads the file again; fails with its errors and keeps what was loaded. Without the file, goes back to `config.h`'s keys. Either way, it sends a `workspace` event with `"change": "reload"`, as i3 does |
| `GET_CONFIG` (type 9) | `config`, the loaded file's text (`""` with `config.h`'s keys), and tidewm's own `path`, where it looks, and `errors`, the last load's |
| `GET_VERSION` | Its `loaded_config_file_name` is the loaded file, or `""` |
| `mode NAME` (a command) | Makes `NAME` the mode, or `default` the top level; fails for a mode the file hasn't |
| `GET_BINDING_MODES` (type 8) | Each mode's name, `default` first |
| `GET_BINDING_STATE` (type 12) | `{"name": ...}`, the mode now |
| `GET_INPUTS` (type 100) | Each input device, its identifier and settings |
| `input NAME SETTING VALUE` (a command) | Sets that on the devices `NAME` names, until a reload |
| The `mode` event | `{"change": NAME, "pango_markup": false}`, when the mode changes |
