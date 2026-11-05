# tidewm's config file

tidewm reads a config file in sway's syntax at startup, and again when it
gets the `reload` command. A binding's action is a list of the IPC
commands in [`ipc.md`](ipc.md), so anything `swaymsg` can do, a key can.

## Where it is

The first of these that's given:

1. `-c FILE` on tidewm's command line;
2. `$XDG_CONFIG_HOME/tidewm/config`;
3. `~/.config/tidewm/config`.

Without the file, `config.h`'s keys and rules work, as in dwl. Once a file
is loaded, its bindings are all the keys there are, and its `for_window`
lines all the rules, and `config.h`'s are left out, so a file wanting dwl's
defaults says them itself.

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
| `bindswitch [--locked] SWITCH:STATE COMMANDS` | As sway's: a switch turning on, off or either (`lid:on`, `lid:off`, `lid:toggle`, and `tablet:` likewise) runs `COMMANDS`; on the lock screen, only a `--locked` one. Not in a mode. `bindswitch lid:on output eDP-1 disable` and `bindswitch --locked lid:off output eDP-1 enable` turn a laptop's panel off while its lid is closed, and its windows move over; like sway, it does so whether or not another output is on |
| `for_window [CRITERIA] COMMANDS` | Runs `COMMANDS` on each new window `CRITERIA` match, as below |
| `output NAME SETTINGS` | Sets up the output called `NAME`, or every output for `*`, as below |
| `input NAME SETTING VALUE` | Sets up the input devices `NAME` names, as below; `input NAME {` … `}` takes a setting a line |
| `mode NAME {` … `}` | A submap: the `bindsym` lines inside are in force while `NAME` is the mode, and the top level's aren't. `NAME` is a word, or text in `"`, without `;`, which ends a command; opening a mode again adds to it |
| `single_window MIN_ASPECT WIDTH` | tidewm's own, tide's SPEC §1: a lone tiled window, in any layout, is centered at `WIDTH` of the window area, from 0.1 to 1, when the area is at least `MIN_ASPECT` as wide as it's high, from 0 to 100. Of several lines, the one with the largest `MIN_ASPECT` the area reaches wins, and none means the whole width. The file's lines replace `config.h`'s `single_window`, which is 80% from 2.1 and 60% from 3.2, as tide's |
| `dim_inactive STRENGTH` | tidewm's own, tide's SPEC §6.2: dims every window but the focused one, from 0, none, to 1, black; tide's is 0.07. Without the line, `config.h`'s `dim_inactive`, 0 as it comes. A monitor showing one window, or a fullscreen one, doesn't dim, and nor do picture-in-picture, video, by the app's content type, and a window `dim disable` exempts ([`ipc.md`](ipc.md)) |
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

A later binding of the same keys replaces an earlier one, though a
`--release` binding is apart from the press one. Holding a key repeats its
press binding, as dwl's keys do. Flags, before the keys:

- **`--release`:** runs when the key comes up, if no other key, pointer
  button or scroll came meanwhile: a tap. The modifiers are as they were
  before the key, so `bindsym --release Super_L ...` is Super tapped
  alone, and not `Super+T`, `Super`+click, `Super`+drag or `Super` with a
  scroll. The window still gets the key, down and up, as in sway. The IPC's
  `binding` event says when a binding runs ([`ipc.md`](ipc.md)), so
  `bindsym --release Super_L nop launcher` is a tap a shell can listen for.
- **`--locked`:** runs on the lock screen too. Other bindings, and all of
  `config.h`'s keys, don't.

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

## Modes

As in sway, the `mode NAME` command makes a mode's bindings the ones in
force, and `mode default` goes back to the top level, so a mode's own
keys must include a way out. Modes don't nest, and `default` can't be
opened. A load, `reload` included, starts at the top level.

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
A value is a word or text in `"`, where `\"` is a quote, and is a POSIX
extended regular expression, matching anywhere unless anchored:

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

As in sway, `output NAME SETTINGS` sets up the output called `NAME`, as
`GET_OUTPUTS` names it, or every output for `*`. An output's own lines
override `*`'s, and a later line for a name adds to an earlier one.
`config.h`'s monitor rules still apply first, for what the file doesn't
say, such as the layout.

| Setting | Does |
|---|---|
| `mode WIDTHxHEIGHT[@HZ]` | The mode, `Hz` or not after the rate, as `2560x1440@144Hz`. It's one the output lists, the fastest at that size if no rate is given, or else a custom mode. `resolution` and `res` say the same |
| `scale S` | The scale, above 0 and up to 10 |
| `position X Y` | Where it is in the layout, in logical pixels; `pos` and `X,Y` work too |
| `transform T` | `normal`, `90`, `180`, `270`, `flipped`, or `flipped-` one of those |
| `enable`, `disable` | Turns it on or off; a disabled output's windows move to the focused one |

The lines apply when an output appears, at startup or when it's plugged
in, and again on `reload`, when each output starts over from `config.h`'s
monitor rule and its preferred mode, so a setting taken out of the file
goes. An output that refuses its settings at startup, as a mode it can't
show, gets its preferred mode instead, and stderr says so. The `output`
command takes the same settings, and sets them at once, until a reload; it
fails for a name no output has. An output enabled again keeps the mode
it had and goes back to the position it was last given; a mode or
position given while it's disabled, by the command or its line, is used
once it's enabled. Enabling or disabling an output wakes it from the power
saving an idle daemon may have put it in.

```
output eDP-1 scale 1.5
output DP-1 mode 2560x1440@144Hz position 1707 0
output HDMI-A-1 disable
```

## Input

As in sway, `input NAME SETTING VALUE` sets up the input devices `NAME`
names, and an `input NAME {` … `}` block takes a `SETTING VALUE` a line.
`NAME`, a word or text in `"`, is one of:

- a device's identifier, as `GET_INPUTS` gives it, such as
  `1:1:AT_Translated_Set_2_keyboard`: its vendor and product numbers,
  then its name, with `_` for each space;
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
`focus next`, its workspace, or `[con_id=N] focus`. A window with neither
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
- **A grant lapses** after `grant_seconds`. A notification's grant,
  `grant --or-recent`, then brings up the app's most recent window,
  unless a window or an activation used it or you moved on; with no
  window, tidewm says so on stderr.

An activation, xdg-activation's or X11's `_NET_ACTIVE_WINDOW`, marks the
window urgent rather than focusing it, unless a grant names its app, with
the guard on or off. With it off, a new window still uses its grant, and
goes to the grant's workspace, though every window takes focus.

## Errors

A file with any error is left out whole, and what was loaded before stays:
the previous file, or `config.h`'s keys. Each error names the file, the
line and what's wrong, as `/path/to/config:12: no setting is called
bindcode`.

- **At startup**, tidewm writes the errors to stderr, then starts with
  `config.h`'s keys, so the file can be fixed and reloaded.
- **On `reload`**, the command fails with the first three errors.
- **Either way**, `GET_CONFIG` lists them all, until a load works.

An output that refuses its settings, as a mode it can't show, is an error
too, though not one in the file, so the rest of the file still loads. On
`reload`, the output keeps what it had, and the command fails, naming it;
at startup, or when it's plugged in, it gets its preferred mode. Either
way, `GET_CONFIG` lists it.

A binding's commands are only checked when the key is pressed. One that
fails is written to stderr, with the binding's keys.

## Over the IPC

| Request | Gives |
|---|---|
| `reload` (a command) | Reads the file again; fails with its errors and keeps what was loaded. Without the file, goes back to `config.h`'s keys |
| `GET_CONFIG` (type 9) | `config`, the loaded file's text (`""` with `config.h`'s keys), and tidewm's own `path`, where it looks, and `errors`, the last load's |
| `GET_VERSION` | Its `loaded_config_file_name` is the loaded file, or `""` |
| `mode NAME` (a command) | Makes `NAME` the mode, or `default` the top level; fails for a mode the file hasn't |
| `GET_BINDING_MODES` (type 8) | Each mode's name, `default` first |
| `GET_BINDING_STATE` (type 12) | `{"name": ...}`, the mode now |
| `GET_INPUTS` (type 100) | Each input device, its identifier and settings |
| `input NAME SETTING VALUE` (a command) | Sets that on the devices `NAME` names, until a reload |
| The `mode` event | `{"change": NAME, "pango_markup": false}`, when the mode changes |
