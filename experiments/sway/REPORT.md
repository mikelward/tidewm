# tide on stock sway: an experiment

Could tide drop tidewm and run on distro-packaged sway with a small helper on sway's
IPC? This directory holds the attempt and its measurements. It is not for merging.

**Verdict: not stock sway.** With the helper, sway passes 222 of tidewm's 628 tests. It
can't see key presses or clicks, can't keep the first window on an empty workspace from
taking focus, and shows a layout tidewm never shows on some window closes. A small sway
fork would close the layout, focus-guard and dimming gaps with three patches, about
1,000 lines in all. tidewm's workspace model, where one workspace spans every monitor,
would still be missing. **Stay on tidewm** unless tide changes that model. If it does,
"sway 1.12 + 3 patches + the helper" is the next prototype to measure, with this
harness.

All numbers below come from one CI run: [sway-experiment run 42517460212][run]. That
run used Debian testing's `sway 1.12` and tidewm at this branch's base, both on wlroots
0.20's headless backend, on the same runner. Earlier runs are quoted where they differ.

[run]: https://github.com/mikelward/tidewm/actions/runs/42517460212

## What was built

- `tidehelper.py` (Python, standard library only) owns the socket tide uses
  (`SWAYSOCK`) and proxies sway's real one. It passes sway's commands through and
  does tidewm's own commands itself: `grant`, `layout tile|threecol|twocol|monocle`,
  `mfact`, `nmaster`, `single_width`, `zoom`, `movestack`, `minimize`, `maximize`,
  `keep_open`, `persist workspace`, `focus recent`, `focus next|prev`, and directional
  focus and move by tide's rule. It also adds tidewm's fields to `GET_TREE`, its
  events (`layout`, `minimize`, `keep_open`, `close_refused`) and the focus guard.
- **Layout by floating.** Every tile is a sway floating window that the helper sizes and
  places with `resize set` and `move absolute position`, all in one `RUN_COMMAND`. That
  way sway applies a re-layout in one transaction, and the boxes match tidewm's pixel
  for pixel, which sway's ppt-based tiling can't. Before an IPC client gets the event
  or reply, the helper waits until the clients have drawn at their new sizes (at most
  200 ms; sway sizes a floating window by what its client commits).
- **Ordering.** Clients get events from the helper, not from sway. The helper handles
  each sway event first (layout, guard) and then fans it out. Each reply waits for a
  `SEND_TICK` marker, so a reply comes after the layout its command caused. This hides
  from IPC clients the race between a window mapping in sway's place and the helper
  moving it. It doesn't hide that race from the screen; see below.
- **Config.** tidewm's own config lines (`focus_guard`, `single_window`,
  `layout_defaults` and so on) go to the helper. A binding that runs a tidewm command
  becomes `nop tide CMD`, and the helper runs CMD from sway's `binding` event, so
  `bindsym Ctrl+w kill` still honors `keep_open`. The guard adds `no_focus [all]`.
- `tidewm-on-sway` stands in for tidewm in `tests/harness.py` (`TIDEWM=...`). It takes
  tidewm's `-c` and `-s` and runs sway with `look.conf` plus the test's config.
  `TIDE_SWAY_MODE=stock` runs sway alone. The harness itself is unchanged.
- `run-suite.py` runs the whole suite in one mode. `bench/bench.py` measures latency,
  intermediate states, frames, layout parity and IPC cost the same way for every mode.
  `summarize.py` makes the tables. `.github/workflows/sway-experiment.yml` runs all
  three modes on this branch, never fails on test failures, and uploads everything.

## Test results

Tests passing per file. A test with subtests counts once per subtest, so the total is
628, not 613.

| File | Tests | tidewm | stock sway | sway + helper |
|---|---|---|---|---|
| test_capture | 5 | 5 | 0 | 0 |
| test_commands | 27 | 27 | 15 | 16 |
| test_config | 66 | 66 | 10 | 15 |
| test_dialogs | 32 | 32 | 7 | 9 |
| test_dimming | 12 | 12 | 0 | 4 |
| test_direction | 12 | 12 | 4 | 6 |
| test_ext_workspace | 6 | 6 | 1 | 1 |
| test_floating | 10 | 10 | 0 | 1 |
| test_focus | 89 | 89 | 3 | 40 |
| test_fullscreen | 15 | 15 | 3 | 8 |
| test_gestures | 5 | 5 | 1 | 1 |
| test_inputs | 22 | 22 | 3 | 3 |
| test_install | 4 | 4 | 4 | 4 |
| test_ipc | 17 | 17 | 6 | 9 |
| test_keep_open | 12 | 12 | 1 | 3 |
| test_layers | 38 | 38 | 0 | 0 |
| test_layouts | 44 | 44 | 0 | 25 |
| test_lock | 5 | 5 | 1 | 1 |
| test_maximize | 17 | 17 | 0 | 1 |
| test_minimize | 10 | 10 | 0 | 5 |
| test_mouse | 14 | 14 | 5 | 5 |
| test_outputs | 46 | 46 | 17 | 21 |
| test_resize | 10 | 10 | 6 | 5 |
| test_rules | 13 | 13 | 1 | 6 |
| test_seats | 9 | 9 | 0 | 0 |
| test_shortcuts | 2 | 2 | 0 | 0 |
| test_split | 27 | 27 | 0 | 8 |
| test_urgency | 9 | 9 | 0 | 5 |
| test_workspaces | 39 | 39 | 14 | 19 |
| test_xwayland | 11 | 11 | 1 | 1 |
| **all** | **628** | **628** | **103** | **222** |

Stock sway: 103 pass, 370 fail, 129 error, 26 time out. With the helper: 222 pass, 287
fail, 92 error, 27 time out. Across four runs the helper scored 209 to 222, and tidewm
626 to 628. Run in parallel, a few timing tests are flaky on every compositor.

### Categories of failure (sway + helper)

These are rough counts, sorted by each failure's error text.

| Category | About | What it is |
|---|---|---|
| Timed out waiting for an event or reply | 68 | Mostly events tidewm sends and sway never does: `capture`, `input`, layer and lock changes, output focus, X11 urgency |
| Workspace and output model | 59 | tidewm's workspace spans every monitor; sway's belong to one output each |
| Config checking | 54 | tidewm rejects bad lines on `reload`; sway takes most, and the helper doesn't check its own yet |
| Error text and reply shape | 37 | tidewm's messages and `parse_error`; cosmetic |
| Input observation | 27 | `last_input`, `prompt_may_focus`, a key or click canceling a grant: sway's IPC has no input events |
| Launcher (`hyprland-focus-grab-v1`) | 22 | sway lacks the protocol tide's launcher uses to hold the keyboard |
| Focus and urgency state | 21 | The lone window taking focus, ordering of marks, held-window edge cases |
| Geometry | 20 | Size hints (a tall or short window pushing rows), clients slow to take their size, monocle's `[n]` |
| Synthetic input | 14 | `switch`, `gesture`, `send_shortcut`, `unlock`: no IPC can make sway do these |
| Other | ~100 | Mostly unported details: rule placement, X11 scaling, resize by pixels |

Stock sway fails the same categories plus every tidewm command, layout and guard test.

## Impossible over IPC, versus not done yet

**Impossible over sway's IPC.** Each of these needs compositor code:

- **Seeing input.** sway sends a `binding` event only for a bound key. A key press or
  click that should cancel a grant, `GET_SEATS`'s `last_input` and the polkit-prompt
  rule can't be seen.
- **Holding the first window on an empty workspace.** `should_focus()` in
  `sway/tree/view.c` focuses the only window on the focused workspace "regardless of
  any no_focus criteria". Keystrokes typed at that moment go to the held window.
- **Placing a window before it's first shown.** `view_map()` adds the window to the
  tree, sends `new`, arranges and commits, all before any IPC client can answer. Every
  layout an IPC helper makes is a second transaction.
- **Tiles that stay their size.** sway sizes a floating window by what its client
  commits, so a tile follows the client's own resizes. No event says so; the helper
  would have to poll. Tiling the sway way would avoid this, but then exact geometry,
  `mfact` in pixels and tidewm's rounding would go.
- **Z-order.** Focusing a floating window raises it (`container_raise_floating`), so a
  focused tile can cover a real floating dialog. sway has no setting to stop it.
- **Laying out a hidden workspace.** Moving a floating window takes it to the workspace
  its output shows (`container_floating_move_to`), so the helper can lay out only shown
  workspaces. A window that opens on a hidden one is placed when you switch there.
- **Workspaces spanning every monitor.** sway's workspace is on one output. The helper
  could fake it with hidden per-output workspaces, but every `workspace`, `move` and
  event would need rewriting, and other sway clients would see the fake.
- **Protocols and synthetic input.** `hyprland-focus-grab-v1`, tidewm's `capture`
  events, layer lists in `GET_OUTPUTS`, `send_shortcut`, `gesture`, `switch`,
  `unlock` and `chvt`.
- **Real dimming.** sway's per-window `opacity` over a dark background stands in for
  `dim_inactive`. It shows the wallpaper through the window rather than darkening it.

**Possible, just not done in this prototype:** checking tidewm's config lines on
`reload`; `GET_BINDINGS`, `GET_NAMED_BINDINGS` and `rebind` (sway takes `bindsym` and
`unbindsym` at run time); `--kept-open` bindings, by binding and unbinding on focus
changes (racy); monocle's `[n]` symbol; window size hints in the layout; tidewm's error
text; the guard's corner cases (portal and polkit classes, status popups, the
`--or-recent` lapse); maximize's interplay with fullscreen; minimize's states.

## Measurements

From `bench/bench.py`: 50 runs each, all on the same runner. Times are in ms, as
median / p95. "Seen" is when the window first appears in the compositor's own tree,
polled about every 0.3 ms past the helper. For sway, that tree is its pending state.

**Latency.**

| Transition | tidewm | stock sway | sway + helper |
|---|---|---|---|
| New window: seen → its `new` event reaches tide | −0.6 / −0.1 | −0.2 / 1.2 | 2.1 / 183 |
| New window: seen → helper has sway's `new` | – | – | −0.2 / −0.1 |
| New window: seen → helper sends its layout | – | – | 1.2 / 2.0 |
| New window: seen → sway acks the layout | – | – | 1.6 / 3.1 |
| New window: seen → final geometry | 0 / 0 | 0 / 0 | 4.4 / 206 |
| Close: gone → final geometry | 0 / 0 | 0 / 0 | 0.8 / 1.2 |
| Workspace switch: command → reply | 0.18 / 0.23 | 0.63 / 0.93 | 2.3 / 3.3 |
| Workspace switch: command → tree settled | 1.6 / 2.3 | 0.8 / 1.2 | 1.0 / 1.3 |
| `focus next`: command → reply | 0.17 / 0.21 | 0.61 / 1.4 | 2.2 / 2.6 |
| Granted window: seen → focused | 0 / 0 | 0 / 0 (no guard) | 2.7 / 3.5 |

The p95 of 183 to 206 ms is the helper's 200 ms cap on waiting for a client to draw at
its new size. In about 1 open in 20, a client takes longer than that. tidewm says where a
window is when it configures it. With a floating window, sway says what the client last
drew.

**Intermediate states and flicker.**

| | tidewm | stock sway | sway + helper |
|---|---|---|---|
| New window: distinct tree states before the final one | 0 | 0 | 3 every time: sway's tiled place, floated, then placed |
| New window: focus changes before settling | 0 | 0 | 1 |
| Close: distinct tree states before the final one | 0 | 0 | 1 every time: a hole where the window was |
| Close: runs with a wrong frame on screen (grim, every frame) | 0 / 25 | 0 / 25 | 3 / 25 here; 15 / 25 and 25 / 25 in earlier runs |
| Open: runs with a wrong frame on screen | 0 / 25 | 0 / 25 | 0 / 25 |
| Held window (guard on): tree samples where it had focus | 0 | ~900 of ~900 (no guard) | 0 |

On open, sway's transaction waits for the new client's first buffer. The helper's float
and move arrive before that, so no wrong frame was seen in 75 opens over three runs.
Close is different: the unmap transaction has nothing to wait for, so the hole where
the window was can reach the screen for one frame (16.7 ms) before the helper fills it.
How often that happens depended on runner timing (3 to 25 of 25). This strip is from a
run where it showed: before, the wrong frame, after.

![close flicker](frames/flicker-close-sway-helper.png)

**Layout parity.** Master/stack with 1 to 5 windows, then closing the first stack
window, then zooming the third:

- The helper matched tidewm exactly in 6 of 7 steps. The miss, at 5 windows, was one
  stack window still 576×240 rather than 576×180: its client hadn't drawn at the new
  size within 200 ms. In an earlier run all steps matched but zoom, which swapped
  rather than moved to the top; that's fixed.
- Stock sway matched only the lone window. It splits the width evenly, e.g. 427/427/426
  for three windows against tidewm's 704 + 576×360 ×2.

**IPC cost** (20 windows; 200 round trips each):

| | tidewm | stock sway | sway + helper |
|---|---|---|---|
| `GET_VERSION` direct | 0.042 / 0.047 | 0.039 / 0.050 | 0.030 / 0.032 |
| `GET_VERSION` through the socket tide uses | 0.041 / 0.056 | 0.053 / 0.079 | 0.142 / 0.175 |
| `GET_TREE` direct | 0.25 / 0.27 | 0.60 / 0.63 | 0.43 / 0.45 |
| `GET_TREE` through the socket tide uses | 0.25 / 0.28 | 0.60 / 0.64 | 1.15 / 1.17 |

The proxy adds about 0.1 ms per request, and 0.7 ms to a 20-window tree, which it
rewrites in Python. Raw numbers: `sway-experiment-*` artifacts, `*-bench.json`.

## Appearance

`look.conf` makes sway look like tide as far as its config goes: no borders, no title
bars, no gaps, no smart borders, tidewm's `#222222` background (`swaybg`), tide's colors
for any border a rule turns on, and no `swaynag`. There's **no `bar` block**: swaybar
isn't part of tide's design. tide's bar is its own Quickshell layer surface, and sway
honors its exclusive zone as tidewm does. With the helper, `dim_inactive` becomes
per-window `opacity`.

Three windows in master/stack, `dim_inactive 0.12`, tidewm then sway + helper:

![tidewm](frames/look-tidewm.png) ![sway + helper](frames/look-sway-helper.png)

These match apart from tidewm's 1-pixel borders (`borderpx`, which tide's config can
turn off) and how dark the dim is. What sway's config can't match:

- **Dim inactive.** Opacity shows what's behind (the wallpaper) rather than darkening.
  tide's 0.12 dim (SPEC §6.2) needs compositor code or SwayFX.
- **Animations.** None in sway. tide has none either, so nothing is lost.
- **Rounded corners and shadows.** Not in sway. tide doesn't use them (SPEC §6.2, and
  its bar has "no corner radius, no shadow"), so nothing is lost.
- **No border at all with a resize grab.** tide drags the master/stack edge from inside
  the tiles. sway resizes floating windows only with a modifier or from a border; the
  helper would need its own drag.

## SwayFX as a base

[SwayFX](https://github.com/WillPower3309/swayfx) is a fork of sway that renders with
scenefx, its own fork of wlroots' scene graph. It adds blur, rounded corners, shadows,
`default_dim_inactive` (a real dim, with colors for unfocused and urgent windows),
`layer_effects`, and scratchpad-as-minimize.

- **What it tracks.** SwayFX 0.6 (2026-08-05) is sway 1.12 on wlroots 0.20 and
  `scenefx-0.5`, with 39 commits on master since. sway's main branch is now 1.13-dev
  on wlroots 0.21.
- **How far it lags.** It rebased onto sway 1.10 (released 2024-10-27) on 2025-02-16,
  3.5 months later. It rebased onto 1.11 (2025-06-08) on 2025-06-24, 2 weeks later, and
  onto 1.12 (2026-05-25) on 2026-07-12, 7 weeks later. So a fork can keep up, but
  SwayFX's diff is large, since it replaces the renderer.
- **For tide.** It brings real dimming and nothing else tide needs. It doesn't help
  with layouts, the focus guard or the workspace model. It also adds a dependency
  Debian doesn't ship (scenefx), and it needs a GLES2 renderer, so the headless
  pixman tests here wouldn't run it as is. A tide fork of sway is better off taking one
  dim patch than basing on SwayFX.

## The fork path: smallest sway patches, ranked

Each patch removes a gap the helper can't close well. Sizes are estimates from sway
1.12's source.

1. **Native master/stack layouts.** Add tide's `tile`, `threecol`, `twocol` and
   `monocle` to sway's layouts: `sway/tree/arrange.c` (`arrange_children()` dispatches
   on the layout), with `layout`, `mfact`, `nmaster`, `single_width`, `zoom` and
   `movestack` in `sway/commands/`. **About 500 to 700 lines.** This removes all the
   floating-tile workarounds: the close flicker, tiles following client sizes, the
   z-order problem, the hidden-workspace deferral, and the 200 ms settle. Upstream:
   no. sway's scope is i3's layouts.
   - A smaller alternative, about 150 lines: hold `view_map()`'s and
     `view_unmap()`'s transaction until a registered IPC manager answers the `new` or
     `close` event, with a timeout like `txn_timeout_ms` (`sway/tree/view.c`,
     `sway/desktop/transaction.c`, `sway/ipc-server.c`). That fixes the flicker but
     keeps the floating workarounds. Upstream: unlikely.
2. **Focus guard support.** A config switch so `no_focus` also covers the lone window,
   about 10 lines in `should_focus()`; upstream might take it as an option. Plus an IPC
   event for "you pressed a key or clicked", without the key, so grants cancel and
   `last_input` works: `sway/input/keyboard.c` (`handle_key_event`),
   `sway/input/seatop_default.c` (button), and `sway/ipc-server.c`; about 80 lines.
   Upstream: unlikely, since it watches input. **About 90 lines.**
3. **Dim inactive.** A `wlr_scene_rect` over each view, shown while unfocused
   (`sway/tree/container.c` and the focus path in `sway/input/seat.c`), plus a
   `dim_inactive` setting and a `dim` command. **About 100 lines.** Or take SwayFX's.
   Upstream: no.
4. **`hyprland-focus-grab-v1`**, for tide's launcher: a port of tidewm's, about 250
   lines. Or change the launcher to an exclusive-keyboard layer surface that closes
   itself on an outside click, with no patch.
5. **tidewm's IPC extras** that need the compositor: layer lists in `GET_OUTPUTS`, the
   `capture` event and `send_shortcut`. About 200 lines.
6. **Workspaces spanning every monitor.** This is a rewrite of sway's workspace model
   (`sway/tree/workspace.c`, `output.c`, `commands/workspace.c`, `move.c`), well over
   1,000 lines, and it would conflict with every rebase. It's not a patch.

**The fork.** "sway 1.12 + patches 1 to 3", about 700 to 900 lines, plus a helper of
about 1,500 lines for grants, minimize, keep_open and tidewm's commands, rebased on
each sway release (about two a year). Patch 4 or a launcher change brings it to tide's
needs, except the workspace model. Compare tidewm: about 15,000 lines of C, already at
628 of 628 on wlroots 0.20.

## Verdict

- **Stock sway + helper: no.** A third of tidewm's behavior passes. The rest includes
  things no IPC client can do: see input, keep focus from the lone new window, place a
  window before it's shown, fix tile sizes. A close can show a hole for one frame.
- **Small sway fork: plausible, not yet better.** Patches 1 to 3 close the layout,
  flicker, guard and dim gaps. But the workspace model, the launcher's protocol and
  tidewm's IPC extras still need patches 4 to 6 or design changes. Patch 6 is the one
  that isn't small.
- **Stay on tidewm**, unless tide changes to per-output workspaces. That's a design
  call for you. If you make it, the next step is that fork, measured with this harness.

## Reproducing

```
make XWAYLAND=-DXWAYLAND XLIBS="xcb xcb-icccm" dwl tests/client tests/workspaces \
    tests/locker tests/power tests/ime tests/pointer tests/launcher tests/keyboard \
    tests/capture tests/x11client
python3 experiments/sway/run-suite.py helper results    # or stock, or tidewm
python3 experiments/sway/bench/bench.py helper results 50
python3 experiments/sway/summarize.py results
```

These need sway, swaybg and grim on PATH. Nothing here was run outside CI: this
container has no wlroots 0.20 and can't run sway or tidewm. Only the helper's geometry
and config translation were checked locally, by hand.
