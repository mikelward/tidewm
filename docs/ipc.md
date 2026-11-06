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
| 3 | `GET_OUTPUTS` | Each output: `id`, `name`, `make`, `model`, `serial`, `active`, `power`, `primary`, `scale`, `transform`, `focused`, `current_workspace`, `rect`, `current_mode`; and tidewm's own `window_area`, the `rect` less what layer surfaces' exclusive zones take, and `layers`, its mapped layer surfaces from the bottom layer up, each a `namespace`, `layer` (`background`, `bottom`, `top` or `overlay`), `rect` and `exclusive_zone`, for tide's doctor to count bars |
| 4 | `GET_TREE` | The root, its outputs, every workspace under each output, and in each the windows of that monitor's part, in `nodes` (tiled) and `floating_nodes` |
| 7 | `GET_VERSION` | `human_readable`, `variant` (`"tidewm"`), `major`, `minor`, `patch`, `loaded_config_file_name` (the loaded config file, or `""`) |
| 8 | `GET_BINDING_MODES` | Each binding mode's name, `default` first ([`config.md`](config.md)) |
| 9 | `GET_CONFIG` | `config`, the loaded config file's text; `included_configs`, as sway's, each file its `include` lines read, as `path` and `raw_config`; and tidewm's own `path` and `errors` ([`config.md`](config.md)) |
| 10 | `SEND_TICK` | `{"success": true}`, after sending the payload in a `tick` event |
| 12 | `GET_BINDING_STATE` | `{"name": ...}`, the binding mode now |
| 100 | `GET_INPUTS` | Each input device, in the order they came: `identifier` (as [`config.md`](config.md)'s input lines name it), tidewm's own `hyprland_name` (the name Hyprland would give it, which those lines take too), `name`, `vendor`, `product`, `type` (`keyboard`, `pointer` or `touchpad`); a pointer's or touchpad's `scroll_factor`; a keyboard's `xkb_layout_names`, `xkb_active_layout_index`, `xkb_active_layout_name`, `repeat_delay` and `repeat_rate`; and a libinput device's `libinput` settings, as sway names them |
| 101 | `GET_SEATS` | sway's one seat: `name` (`seat0`), `capabilities`, `focus` (the focused window's id, or 0) and `devices`, as `GET_INPUTS` lists them; and tidewm's own `last_input`, your last key press or click, `{"type": "key"` or `"button", "window": ..., "msec_ago": ...}`, or null before any. A key goes to the window with the keyboard, a click to the one with the pointer, which a held button keeps, so a second button goes where the first did; `window` is null for neither, as a key in a launcher, or for input a binding takes. tide's polkit agent lets its prompt take focus only right after your input went to the window asking. Also tidewm's own `prompt_may_focus`: whether a polkit prompt opening now may take the keyboard, by the focus guard's rule for a polkit agent's window, a key pressed within `prompt_seconds` ([`config.md`](config.md)), for `tide prompt-focus` and a shell's agent that draws its own prompt |
| 200 | `GET_BINDINGS` | tidewm's own, as sway has none: each of the config file's bindings, for a shell's list of keyboard shortcuts, as `type` (`key` or `switch`), `keys` (as written, after its variables, as `Mod4+Return`, or a switch's `lid:on`, `tablet:toggle` and the like), `command`, `mode` (a `bindsym`'s mode, `default` for the top level and for every `bindswitch`), `release` and `locked`. Each mode's `bindsym` lines come in the file's order, the top level's first, then the `bindswitch` lines. `config.h`'s keys aren't listed, so without a file there are none |

Any other type gets `{"success": false, "error": "unsupported message type"}`.

### Commands

A `RUN_COMMAND` payload holds commands separated by `;`, but not one in
double quotes, as in sway. A `'` is no quote, so a workspace can be named
`Bob's tasks`, and tidewm's quotes take no escapes. An `exec` command is different: it
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
| `workspace NAME` | Shows that workspace on every monitor, making it if it's new. The rest of the line is the name, spaces and all, or it's in double quotes, which may hold a `;`; a name in quotes can't hold a `"`, and is a name even as `"back_and_forth"` |
| `workspace number N` | The same, for the workspace named `N` |
| `workspace back_and_forth` | Shows the previous workspace |
| `move [container\|window] [to] workspace NAME` | Moves the window to that workspace's part on its own monitor, making it if it's new. `number N` and `back_and_forth` work here too |
| `move [container\|window] [to] output OUTPUT` | Moves the window to another monitor's part of its workspace. `OUTPUT` is a name, or `left`, `right`, `up` or `down` of the focused one |
| `focus output OUTPUT` | Focuses that monitor |
| `[con_id=N] focus` | Shows the window's workspace and focuses it |
| `focus next\|prev` | Focuses the next or previous window on the focused monitor, in stack order, going round |
| `kill` | Asks the window to close |
| `send_shortcut MODS+KEY` | tidewm's own, as Hyprland's `sendshortcut`: sends the window a press and release of `KEY`, such as `ctrl+v`, with the modifiers `ctrl`, `shift`, `alt` or `super` held, whether or not it's focused, as tide's picker pastes. `KEY` is a key name, ignoring case, sent as the key that gives it on the keyboard's layout now, at its lowest level, so a shifted one, as `plus` on a US keyboard, wants `shift` too; focus stays where it was. It fails while the screen is locked |
| `exec COMMAND` | Runs the rest of the payload, `;` and all, with `/bin/sh -c`, in a session of its own. sway ends it at an unquoted `;`; tidewm leaves the shell to read its own quotes |
| `reload` | Reads the config file again ([`config.md`](config.md)); fails with its errors, keeping what was loaded |
| `mode NAME` | Makes the config file's mode `NAME`'s bindings the ones in force; `mode default` goes back to the top level. `NAME` may be in `"` |
| `output NAME SETTINGS` | Sets the output `NAME`'s, or every output's for `*`, mode, scale, position, transform, or `enable` or `disable`, as the config file's output lines do ([`config.md`](config.md)), until a reload |
| `output NAME power on\|off\|toggle` | As sway's: turns the output, or every one for `*`, off as an idle daemon does over wlr-output-power-management, keeping its windows and place, or on again. A disabled output stays off, and the command fails for it alone. `GET_OUTPUTS`'s `power` says which are on |
| `input NAME SETTING VALUE` | Sets one setting, such as `xkb_layout` or `tap`, on the input devices `NAME` names, as the config file's input lines do ([`config.md`](config.md)), until a reload. It fails for a name no device has |
| `input NAME xkb_switch_layout next\|prev\|INDEX` | As sway's: switches the active layout of the keyboards `NAME` names, and of the others in their groups, which share it, now, as a layout indicator's click might. It fails for an index past a keymap's layouts |
| `dim enable\|disable\|toggle` | tidewm's own, tide's SPEC §6.2: whether the window dims when it isn't focused, as `dim_inactive` has windows do ([`config.md`](config.md)); a `for_window` rule's `dim disable` exempts a screen share's preview, say |
| `sticky enable\|disable\|toggle` | As sway's: a sticky floating window is on every workspace, going along to the current one; a tiled one stays where it is. Picture-in-picture is sticky as it opens |
| `floating enable\|disable\|toggle` | Floats or tiles the window. A fullscreen window is left as it is, and the command fails. Floated, it goes where one of its app's floating windows last opened, moved or was resized to, as tide's SPEC §6.4 has it, unless a `for_window` rule floats it as it opens, which puts it in the middle |
| `fullscreen [enable\|disable\|toggle]` | Makes the window fullscreen or not; alone, it toggles. A window the focus guard holds goes fullscreen once it has focus ([`config.md`](config.md)), and until then its request counts as made, so `disable` or `toggle` takes it back. Another window on its workspace taking focus on that monitor ends it, as it ends a maximize, unless that's its dialog |
| `maximize [enable\|disable\|toggle]` | tidewm's own, tide's SPEC §6.3: makes the window cover its monitor's window area, above the other tiles, which stay where they are, with the bar still showing; alone, it toggles. It lasts until it's turned off, the window closes, or another window on its workspace takes focus on that monitor (not a dialog of it). An app's own request maximizes it too. Fullscreen covers it while it lasts, and a floating window gets its place back after |
| `minimize [enable\|disable\|toggle]` | tidewm's own, tide's SPEC §6.7: hides the window, which leaves the layout, and focus on it goes to the top window left on the focused monitor; alone, it toggles. It keeps its workspace, output and states, so a workspace with one stays while it waits, and the window may be moved or closed meanwhile. Focusing it by any means, `focus`, a grant or a key, brings it back first, as `minimize disable` does: it joins the end of its part's stack, as an arriving window does. An app's own minimize request does nothing |
| `layout tile\|threecol\|twocol\|monocle\|floating\|toggle` | tidewm's own: the layout of the current workspace's part on the focused monitor, from `config.h`'s `layouts`, or back to the previous one. The names are tide's: `threecol` centers the master with a stack either side, and `twocol` puts the masters side by side with the rest stacked on the right. Each part keeps its own layouts, and each layout its own `mfact` and `nmaster` (below) |
| `nmaster N\|+N\|-N` | tidewm's own: how many windows the master area holds on the focused monitor |
| `mfact F\|+F\|-F` | tidewm's own: the master area's share of the focused monitor's width, from 0.1 to 0.9. Only a finite number will do |
| `zoom` | tidewm's own: swaps the focused window with the master, or with the next window if it's the master |
| `switch lid\|tablet on\|off` | tidewm's own: moves a switch device of tidewm's own, as if a switch such as the lid had moved, which runs the config file's `bindswitch` lines for it ([`config.md`](config.md)): for a test, which has no lid, or a lid the hardware doesn't report |
| `unlock` | tidewm's own, tide's SPEC §10: drops a lock whose screen locker has gone, as after it crashed, showing the windows again and focusing the top one: from a TTY (`swaymsg -s "$XDG_RUNTIME_DIR"/tidewm-ipc.*.sock unlock`), the way back in that keeps the session. Until then the screen stays covered, no key or pointer event reaches a window, and a new locker may take the lock over. It fails while a locker holds the lock, which only that locker may end, and with nothing locked |
| `chvt N` | tidewm's own: switches to virtual terminal `N`; fails without a session, as on the headless backend |
| `rename workspace [OLD] to NEW` | Renames the current workspace, or `OLD`. Either name may be in double quotes, as for `workspace`; an `OLD` with ` to ` in it must be |
| `persist workspace NAME` | tidewm's own: keeps that workspace, making it if it's new, even while it's empty. `NAME` may be in double quotes, as for `workspace` |
| `unpersist workspace NAME` | tidewm's own: lets it go once it's empty and not current. `NAME` may be in double quotes, as for `workspace` |
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
none), `visible`, `fullscreen_mode` (0 or 1), `sticky`, `maximized` and `minimized` (tidewm's own),
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
| `output` | `0x80000001` | An output appears or goes: `{"change": "unspecified"}`. And tidewm's own: focus moves to another output, by the pointer, a click, a key or a command, or as the focused one goes: `{"change": "focus", "output": NAME}`, null with none left. Quickshell's I3 module asks for the outputs again on any output event, so its focused monitor follows |
| `mode` | `0x80000002` | The binding mode changes: `{"change": NAME, "pango_markup": false}` |
| `window` | `0x80000003` | A window is `new`, `close`d, gets `focus`, changes `title`, `fullscreen_mode`, `floating` or `urgent`, is maximized or not (tidewm's own `maximize`), is minimized or brought back (tidewm's own `minimize`), or moves to another workspace or output, by a command, a key or a drag (`move`): `{"change": ..., "container": ...}`. A window's first event is `new`, sent once it has an output, workspace and floating state |
| `binding` | `0x80000005` | A config file's binding runs, as in sway: `{"change": "run", "binding": {"command": ..., "event_state_mask": [...], "input_code": 0, "symbol": ..., "input_type": "keyboard"}}`, the modifiers named `Shift`, `Lock`, `Ctrl` and `Mod1` to `Mod5`, and the symbol as the binding has it. A `bindswitch` line's is `"input_type": "switch"`, with no modifiers and a null symbol. `config.h`'s keys run no command and send none |
| `shutdown` | `0x80000006` | tidewm exits: `{"change": "exit"}` |
| `tick` | `0x80000007` | Someone sends `SEND_TICK`: `{"first": false, "payload": ...}`. Subscribing sends one at once, with `first` true |
| `capture` | `0x8000001f` | tidewm's own, tide's SPEC §12: a capture session over ext-image-copy-capture starts or stops, as a screen share's or a screenshot's: `{"change": "start"\|"stop", "source": "window"\|"output", "con_id": ...}`, `con_id` the captured window's `id`, or null for an output, which wlroots doesn't say which of. A session stops when its client ends it or goes, or its window closes. wlr-screencopy's captures send none, as wlroots says nothing of them |
| `layout` | `0x8000001e` | tidewm's own: the current workspace's part on a monitor changes layout, or the current workspace changes, or a monitor takes its part of it, as it appears, or is enabled after the workspace changed; a monitor powered off says so once it's on again: `{"change": "layout", "workspace": ..., "output": ..., "layout": ..., "symbol": ...}`, `symbol` as `config.h` gives it, or monocle's `[n]` |
| `input` | `0x80000015` | An input device is `added` or `removed`, a keyboard's keymap changes (`xkb_keymap`), a libinput device's settings are set (`libinput_config`), or a keyboard's active layout switches, by a key, `xkb_switch_layout` or a client (`xkb_layout`): `{"change": ..., "input": ...}`, the device as `GET_INPUTS` has it |

A client that stops reading is closed once 16 MiB of events wait for it.
