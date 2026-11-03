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

Each message is the six bytes `i3-ipc`, a 32-bit payload length and a 32-bit
type, both in the machine's byte order, then the payload. A message without
the magic, or over 4 MiB, closes that connection only.

## Requests

| Type | Name | Reply |
|---|---|---|
| 0 | `RUN_COMMAND` | One result per command, as below |
| 1 | `GET_WORKSPACES` | `[]` until tidewm has workspaces (the pool in `TODO.md`) |
| 2 | `SUBSCRIBE` | `{"success": true}`, or `false` for anything but a JSON array of the event names below |
| 3 | `GET_OUTPUTS` | Each output: `id`, `name`, `make`, `model`, `serial`, `active`, `power`, `primary`, `scale`, `transform`, `focused`, `current_workspace` (null for now), `rect`, `current_mode` |
| 4 | `GET_TREE` | The root, its outputs, and each output's windows in `nodes` (tiled) and `floating_nodes` |
| 7 | `GET_VERSION` | `human_readable`, `variant` (`"tidewm"`), `major`, `minor`, `patch`, `loaded_config_file_name` |
| 10 | `SEND_TICK` | `{"success": true}`, after sending the payload in a `tick` event |

Any other type gets `{"success": false, "error": "unsupported message type"}`.

### Commands

A `RUN_COMMAND` payload holds commands separated by `;`. Each gets a result,
`{"success": true}` or `{"success": false, "parse_error": true, "error":
"Unknown command: ..."}`.

| Command | Does |
|---|---|
| `nop` | Nothing |
| `exit` | Sends the `shutdown` event, then exits |

### Windows

A window in the tree or in an event is a container: `id` (unique for
tidewm's life), `type` (`con`, or `floating_con`), `name` (the title),
`app_id`, `pid`, `shell` (`xdg_shell` or `xwayland`), `focused`, `urgent`,
`visible`, `fullscreen_mode` (0 or 1), `rect` (in layout coordinates, border
included) and `foreign_toplevel_identifier`, the ext-foreign-toplevel-list
identifier `grim -T` takes. An X11 window has `app_id` null and its class,
instance and title in `window_properties`, as in sway, and its X11 window id
in `window`. Unmanaged X11 windows (menus, tooltips) aren't listed.

## Events

| Event | Type | Sent when |
|---|---|---|
| `output` | `0x80000001` | An output appears or goes: `{"change": "unspecified"}` |
| `window` | `0x80000003` | A window is `new`, `close`d, gets `focus`, or changes `title`, `fullscreen_mode`, `floating` or `urgent`: `{"change": ..., "container": ...}`. A window's first event is `new`, sent once it has an output and its floating state |
| `shutdown` | `0x80000006` | tidewm exits: `{"change": "exit"}` |
| `tick` | `0x80000007` | Someone sends `SEND_TICK`: `{"first": false, "payload": ...}`. Subscribing sends one at once, with `first` true |

`workspace` (`0x80000000`) can be subscribed to, but sends nothing until
tidewm has workspaces. A client that stops reading is closed once 16 MiB of
events wait for it.
