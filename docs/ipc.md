# tidewm's IPC

tidewm speaks sway's IPC: the framing and JSON of
[sway-ipc(7)](https://man.archlinux.org/man/sway-ipc.7), which is i3's. It
answers the subset tide needs, so `swaymsg`, Quickshell's I3 module and
other clients of sway's socket work against it.

## The socket

tidewm listens on `$XDG_RUNTIME_DIR/tidewm-ipc.<uid>.<pid>.sock` and names
it in `SWAYSOCK` before running the startup command (`-s`), so everything
started from it finds the socket. Without `XDG_RUNTIME_DIR` there is no
socket: tidewm says so on stderr and runs without one. The socket is
removed when tidewm exits.

## Workspaces

tidewm's workspaces aren't sway's. A workspace has a name, and spans every
monitor: each monitor shows its own part of it, the windows whose output it
is. One workspace is current, on every monitor at once. A workspace is made
the first time a command names it, and goes once it's empty and not
current, unless it's persistent. tidewm starts on workspace `1`.

So `GET_WORKSPACES` lists each workspace once, its `output` the focused
one, and `focused` and `visible` both mean it's current. Every output's
`current_workspace` is the same.

## Framing

Each message is the six bytes `i3-ipc`, a 32-bit payload length and a 32-bit
type, both in the machine's byte order, then the payload. A message without
the magic, or over 4 MiB, closes that connection only.

## Requests

| Type | Name | Reply |
|---|---|---|
| 0 | `RUN_COMMAND` | One result per command, as below |
| 1 | `GET_WORKSPACES` | Each workspace, in the order made: `id`, `name`, `num` (the name as a number, or -1), `focused`, `visible`, `urgent` (a window on it is), `persistent`, `output`, `rect`, and tidewm's own `layout`, its part's on the focused monitor (`null` for one it hasn't had yet) |
| 2 | `SUBSCRIBE` | `{"success": true}`, or `false` for anything but a JSON array of the event names below |
| 3 | `GET_OUTPUTS` | Each output: `id`, `name`, `make`, `model`, `serial`, `active`, `power`, `primary`, `scale`, `transform`, `focused`, `current_workspace`, `rect`, `current_mode` |
| 4 | `GET_TREE` | The root, its outputs, every workspace under each output, and in each the windows of that monitor's part, in `nodes` (tiled) and `floating_nodes` |
| 7 | `GET_VERSION` | `human_readable`, `variant` (`"tidewm"`), `major`, `minor`, `patch`, `loaded_config_file_name` (the loaded config file, or `""`) |
| 8 | `GET_BINDING_MODES` | Each binding mode's name, `default` first ([`config.md`](config.md)) |
| 9 | `GET_CONFIG` | `config`, the loaded config file's text, and tidewm's own `path` and `errors` ([`config.md`](config.md)) |
| 10 | `SEND_TICK` | `{"success": true}`, after sending the payload in a `tick` event |
| 12 | `GET_BINDING_STATE` | `{"name": ...}`, the binding mode now |
| 100 | `GET_INPUTS` | Each input device, in the order they came: `identifier` (as [`config.md`](config.md)'s input lines name it), `name`, `vendor`, `product`, `type` (`keyboard`, `pointer` or `touchpad`); a keyboard's `xkb_layout_names`, `xkb_active_layout_index`, `xkb_active_layout_name`, `repeat_delay` and `repeat_rate`; and a libinput device's `libinput` settings, as sway names them |
| 101 | `GET_SEATS` | sway's one seat: `name` (`seat0`), `capabilities`, `focus` (the focused window's id, or 0) and `devices`, as `GET_INPUTS` lists them; and tidewm's own `last_input`, your last key press or click, `{"type": "key"` or `"button", "window": ..., "msec_ago": ...}`, or null before any. A key goes to the window with the keyboard, a click to the one with the pointer, which a held button keeps, so a second button goes where the first did; `window` is null for neither, as a key in a launcher, or for input a binding takes. tide's polkit agent lets its prompt take focus only right after your input went to the window asking |

Any other type gets `{"success": false, "error": "unsupported message type"}`.

### Commands

A `RUN_COMMAND` payload holds commands separated by `;`, quotes and all, so
a workspace can be named `Bob's tasks`. An `exec` command is different: it
takes the rest of the payload, `;` included, and its shell gets it as
written, so put `exec` last. Empty commands are skipped. Each gets a
result:
`{"success": true}`, `{"success": false, "error": ...}` for one that can't be
done, or that plus `"parse_error": true` for one tidewm doesn't know.

A command that acts on a window acts on the focused one, unless it starts
with criteria picking another: `[con_id=N]`, a container's `id`.

| Command | Does |
|---|---|
| `nop` | Nothing |
| `exit` | Sends the `shutdown` event, then exits |
| `workspace NAME` | Shows that workspace on every monitor, making it if it's new. The rest of the line is the name, spaces and all |
| `workspace number N` | The same, for the workspace named `N` |
| `workspace back_and_forth` | Shows the previous workspace |
| `move [container\|window] [to] workspace NAME` | Moves the window to that workspace's part on its own monitor, making it if it's new. `number N` and `back_and_forth` work here too |
| `move [container\|window] [to] output OUTPUT` | Moves the window to another monitor's part of its workspace. `OUTPUT` is a name, or `left`, `right`, `up` or `down` of the focused one |
| `focus output OUTPUT` | Focuses that monitor |
| `[con_id=N] focus` | Shows the window's workspace and focuses it |
| `focus next\|prev` | Focuses the next or previous window on the focused monitor, in stack order, going round |
| `kill` | Asks the window to close |
| `exec COMMAND` | Runs the rest of the payload, `;` and all, with `/bin/sh -c`, in a session of its own. sway ends it at an unquoted `;`; tidewm leaves the shell to read its own quotes |
| `reload` | Reads the config file again ([`config.md`](config.md)); fails with its errors, keeping what was loaded |
| `mode NAME` | Makes the config file's mode `NAME`'s bindings the ones in force; `mode default` goes back to the top level. `NAME` may be in `"` |
| `output NAME SETTINGS` | Sets the output `NAME`'s, or every output's for `*`, mode, scale, position, transform, or `enable` or `disable`, as the config file's output lines do ([`config.md`](config.md)), until a reload |
| `input NAME SETTING VALUE` | Sets one setting, such as `xkb_layout` or `tap`, on the input devices `NAME` names, as the config file's input lines do ([`config.md`](config.md)), until a reload. It fails for a name no device has |
| `dim enable\|disable\|toggle` | tidewm's own, tide's SPEC §6.2: whether the window dims when it isn't focused, as `dim_inactive` has windows do ([`config.md`](config.md)); a `for_window` rule's `dim disable` exempts a screen share's preview, say |
| `sticky enable\|disable\|toggle` | As sway's: a sticky floating window is on every workspace, going along to the current one; a tiled one stays where it is. Picture-in-picture is sticky as it opens |
| `floating enable\|disable\|toggle` | Floats or tiles the window. A fullscreen window is left as it is, and the command fails. Floated, it goes where one of its app's floating windows last opened, moved or was resized to, as tide's SPEC §6.4 has it, unless a `for_window` rule floats it as it opens, which puts it in the middle |
| `fullscreen [enable\|disable\|toggle]` | Makes the window fullscreen or not; alone, it toggles. A window the focus guard holds goes fullscreen once it has focus ([`config.md`](config.md)), and until then its request counts as made, so `disable` or `toggle` takes it back. Another window on its workspace taking focus on that monitor ends it, as it ends a maximize, unless that's its dialog |
| `maximize [enable\|disable\|toggle]` | tidewm's own, tide's SPEC §6.3: makes the window cover its monitor's window area, above the other tiles, which stay where they are, with the bar still showing; alone, it toggles. It lasts until it's turned off, the window closes, or another window on its workspace takes focus on that monitor (not a dialog of it). An app's own request maximizes it too. Fullscreen covers it while it lasts, and a floating window gets its place back after |
| `layout tile\|threecol\|twocol\|monocle\|floating\|toggle` | tidewm's own: the layout of the current workspace's part on the focused monitor, from `config.h`'s `layouts`, or back to the previous one. The names are tide's: `threecol` centers the master with a stack either side, and `twocol` puts the masters side by side with the rest stacked on the right. Each part keeps its own layouts, and each layout its own `mfact` and `nmaster` (below) |
| `nmaster N\|+N\|-N` | tidewm's own: how many windows the master area holds on the focused monitor |
| `mfact F\|+F\|-F` | tidewm's own: the master area's share of the focused monitor's width, from 0.1 to 0.9. Only a finite number will do |
| `zoom` | tidewm's own: swaps the focused window with the master, or with the next window if it's the master |
| `chvt N` | tidewm's own: switches to virtual terminal `N`; fails without a session, as on the headless backend |
| `rename workspace [OLD] to NEW` | Renames the current workspace, or `OLD` |
| `persist workspace NAME` | tidewm's own: keeps that workspace, making it if it's new, even while it's empty |
| `unpersist workspace NAME` | tidewm's own: lets it go once it's empty and not current |
| `urgent enable\|disable` | Marks the window urgent, as the most recent mark, or takes its mark away, as in sway. sway's `allow` and `deny` aren't supported: every window may be marked |
| `grant [--pid PID] [--key KEY] [--workspace NAME] [APP ...]` | tidewm's own: a one-shot grant for the focus guard ([`config.md`](config.md)). The first new window of one of the `APP`s takes focus, or of any app for `*`, which stands alone; `APP`s match ignoring case and a `.desktop`, and a bare name matches a qualified ID's last part, as `files` matches `org.example.Files`. With `--pid`, a window whose process descends from `PID` takes it too, and `APP` may be left out. `--workspace` takes the window to that workspace, made if it's new, with focus. `--key` names the grant for `grant extend`. Options take `--opt VALUE` or `--opt=VALUE` |
| `grant --or-recent APP ...` | tidewm's own: a notification's click, as tide's shell grants one. The same one-shot grant for the `APP`s, which, if it lapses unused and you've done nothing else, brings up the most recently focused window of any of them, wherever it is (or, with none ever focused, any of them) |
| `focus recent APP ...` | tidewm's own: brings up the most recently focused window of any of the `APP`s at once, as a notification center's entry does once its notification is gone. It's you moving on, so it cancels every grant. It fails for an app with no window |
| `grant extend --key KEY [--pid PID] APP ID ...` | tidewm's own: adds `ID`s to the grant made with that `KEY`, `PID` and first `APP`, if it still holds, for the app IDs a slow lookup finds; nothing, if it's gone |

### Layouts

Each workspace's part on each monitor has its own layout, and each layout
there its own `mfact` and `nmaster`, as in tide (its SPEC §6.1). The
`layout`, `mfact` and `nmaster` commands set them for the current
workspace's part on the focused monitor, and a workspace switch gives each
monitor its part of the new one; a disabled monitor takes its part once
it's enabled. A part starts from its monitor's aspect: `threecol` from 2.1
wide (21:9 and wider), otherwise the layout of `config.h`'s monitor rule.
A part is kept by output name, so a monitor plugged in again finds it, and
a workspace that goes takes its parts with it: one made again starts
afresh.

### Windows

A new window joins the end of its monitor's stack, so the master stays
put, as in tide. A window moved in from another workspace or monitor, or
tiled again after floating, counts as new. `zoom` makes a window the
master.

With the focus guard ([`config.md`](config.md)), a new window no grant
let in opens unfocused, with `urgent` true and an `urgent` event. A
window's activation marks it urgent, with or without the guard, unless a
grant names its app. With only windows the guard holds on the focused
monitor, `focus next|prev` goes to one.

A window is marked urgent by its activation, the focus guard holding it,
X11's urgency hint coming on, or `urgent enable`, which a shell uses for
its own marks, as from a notification. Each mark is the most recent, even
on a window already marked, so a shell can list marked windows most
recent first by `urgent_order`. Focusing a window, or `urgent disable`,
takes its mark away. Either way, the window has an `urgent` event.

A window in the tree or in an event is a container: `id` (unique for
tidewm's life), `type` (`con`, or `floating_con`), `name` (the title),
`app_id`, `pid`, `shell` (`xdg_shell` or `xwayland`), `focused`, `urgent`,
`urgent_order` (tidewm's own: higher for a more recent mark, 0 if it has
none), `visible`, `fullscreen_mode` (0 or 1), `sticky`, `maximized` (tidewm's own),
`dim` and `dimmed` (tidewm's own: whether it dims when it isn't focused, and is dimmed now),
`rect` (in layout coordinates, border included), `workspace` and `output` (tidewm's own: where it is) and
`foreign_toplevel_identifier`, the ext-foreign-toplevel-list
identifier `grim -T` takes. An X11 window has `app_id` null and its class,
instance and title in `window_properties`, as in sway, and its X11 window id
in `window`. Unmanaged X11 windows (menus, tooltips) aren't listed.

## Events

| Event | Type | Sent when |
|---|---|---|
| `workspace` | `0x80000000` | A workspace is made (`init`), goes (`empty`), becomes current (`focus`, with the previous one in `old`), is renamed (`rename`), or a window on it gains or loses urgency (`urgent`): `{"change": ..., "current": ..., "old": ...}` |
| `output` | `0x80000001` | An output appears or goes: `{"change": "unspecified"}` |
| `mode` | `0x80000002` | The binding mode changes: `{"change": NAME, "pango_markup": false}` |
| `window` | `0x80000003` | A window is `new`, `close`d, gets `focus`, changes `title`, `fullscreen_mode`, `floating` or `urgent`, is maximized or not (tidewm's own `maximize`), or moves to another workspace or output, by a command, a key or a drag (`move`): `{"change": ..., "container": ...}`. A window's first event is `new`, sent once it has an output, workspace and floating state |
| `binding` | `0x80000005` | A config file's binding runs, as in sway: `{"change": "run", "binding": {"command": ..., "event_state_mask": [...], "input_code": 0, "symbol": ..., "input_type": "keyboard"}}`, the modifiers named `Shift`, `Lock`, `Ctrl` and `Mod1` to `Mod5`, and the symbol as the binding has it. `config.h`'s keys run no command and send none |
| `shutdown` | `0x80000006` | tidewm exits: `{"change": "exit"}` |
| `tick` | `0x80000007` | Someone sends `SEND_TICK`: `{"first": false, "payload": ...}`. Subscribing sends one at once, with `first` true |
| `layout` | `0x8000001e` | tidewm's own: the current workspace's part on a monitor changes layout, or the current workspace changes, or a monitor takes its part of it, as it appears, or is enabled after the workspace changed; a monitor powered off says so once it's on again: `{"change": "layout", "workspace": ..., "output": ..., "layout": ..., "symbol": ...}`, `symbol` as `config.h` gives it, or monocle's `[n]` |
| `input` | `0x80000015` | An input device is `added` or `removed`, a keyboard's keymap changes (`xkb_keymap`), or a libinput device's settings are set (`libinput_config`): `{"change": ..., "input": ...}`, the device as `GET_INPUTS` has it. A switch of the active layout doesn't send one yet |

A client that stops reading is closed once 16 MiB of events wait for it.
