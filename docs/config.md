# tidewm's config file

tidewm reads a config file in sway's syntax at startup, and again when it
gets the `reload` command. A binding's action is a list of the IPC
commands in [`ipc.md`](ipc.md), so anything `swaymsg` can do, a key can.
Outputs and input follow (see `TODO.md`).

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
supported yet.

| Setting | Does |
|---|---|
| `set $NAME VALUE` | Makes `$NAME` stand for the rest of the line in every later line. A name is letters, digits and `_`. Where names overlap, as `$w` and `$wide`, the longest that matches wins, as in sway |
| `bindsym KEYS COMMANDS` | Pressing `KEYS` runs `COMMANDS`, IPC commands separated by `;`. An `exec` takes the rest of the line, `;` and all, so it comes last |
| `for_window [CRITERIA] COMMANDS` | Runs `COMMANDS` on each new window `CRITERIA` match, as below |
| `mode NAME {` … `}` | A submap: the `bindsym` lines inside are in force while `NAME` is the mode, and the top level's aren't. `NAME` is a word, or text in `"`, without `;`, which ends a command; opening a mode again adds to it |

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

- **`--release`:** runs when the key comes up, if no other key went down
  meanwhile: a tap. The modifiers are as they were before the key, so
  `bindsym --release Super_L ...` is Super tapped alone. The window still
  gets the key, down and up, as in sway.
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

## Errors

A file with any error is left out whole, and what was loaded before stays:
the previous file, or `config.h`'s keys. Each error names the file, the
line and what's wrong, as `/path/to/config:12: no setting is called
bindcode`.

- **At startup**, tidewm writes the errors to stderr, then starts with
  `config.h`'s keys, so the file can be fixed and reloaded.
- **On `reload`**, the command fails with the first three errors.
- **Either way**, `GET_CONFIG` lists them all, until a load works.

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
| The `mode` event | `{"change": NAME, "pango_markup": false}`, when the mode changes |
