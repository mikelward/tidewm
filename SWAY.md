# tidewm or sway

Should tide drop tidewm, its dwl fork, and run on sway instead? We tried in November
2026. This records what we found and what was decided. The full report, the helper and
the benchmark harness are on the `claude/sway-experiment` branch, in
[`experiments/sway/`][branch], and in [draft PR #73](https://github.com/mikelward/tidewm/pull/73), which isn't for merging.

[branch]: https://github.com/mikelward/tidewm/tree/claude/sway-experiment/experiments/sway

## Where it stands

**tidewm for now**, 2026-11-08. Both paths are forks, and both can keep tide's
workspaces spanning every monitor; the choice is which costs less to carry.

- **tidewm**, a fork of dwl, exists: workspaces span every monitor, and it passes all
  628 of its tests. dwl changes slowly, and tidewm's code sits in its own files, so
  rebases are cheap. All of it is ours to maintain.
- **A sway fork** doesn't exist yet. It needs patches 1 to 3 below, about 700 to 900
  lines, plus workspaces spanning every monitor. The experiment sized that as a rewrite
  of sway's workspace code, well over 1,000 lines. Grouping each output's workspaces so
  they switch together might do it in a few hundred, unmeasured. It brings sway's
  maturity and tool compatibility, and patches in code that moves faster than dwl's.

The next step, if wanted, is prototyping that fork and measuring it against tidewm with
the same harness. Otherwise revisit after tidewm has been used for real.

## Why

**Stock sway plus a helper can't do it.** The helper owned tide's IPC socket, proxied
sway's, and did tidewm's own commands, layouts and focus guard itself. It passed 222
of tidewm's 628 tests, against 103 for sway alone. tidewm passed all 628, in the same
CI run on wlroots 0.20's headless backend.

No IPC client can do these on sway:

- see key presses and clicks, which cancel a focus grant;
- keep the lone new window on an empty workspace from taking focus, since sway
  focuses it whatever `no_focus` says;
- place a window before it's first shown, so every layout is a second step;
- keep a tile its size, or keep a focused tile from covering a floating dialog;
- lay out a workspace that isn't shown;
- span one workspace over every monitor;
- offer `hyprland-focus-grab-v1`, which tide's launcher uses.

**It shows.** Medians and 95th percentiles over 50 runs:

| | tidewm | sway + helper |
|---|---|---|
| New window to its final place | 0 / 0 ms | 4.4 / 206 ms |
| Layouts a new window passes through before its own | 0 | 3 |
| Closes where a frame shows a hole where the window was | 0 of 25 | 3 to 25 of 25 |
| A window the guard holds ever focused | never | never |
| `GET_TREE` with 20 windows | 0.25 ms | 1.15 ms, through the helper |

The 206 ms is the helper's cap on waiting for a client to draw at its new size.

**Appearance isn't the problem.** Without swaybar, which tide doesn't use, and with no
borders, title bars or gaps, sway looks like tidewm. Only `dim_inactive` needs code:
sway's `opacity` shows the wallpaper through a window rather than darkening it.

## A small sway fork

These patches would close the gaps, smallest first by what they buy. Sizes are
estimates from sway 1.12's source. Upstream would likely refuse all of them but the
first half of the second.

1. tide's layouts (`tile`, `threecol`, `twocol`, `monocle`) native in
   `sway/tree/arrange.c`, with their commands: about 500 to 700 lines. This removes the
   flicker, the settle wait and the floating workarounds.
2. Focus-guard support: `no_focus` covering the lone window, about 10 lines, plus an
   IPC event for a key press or click, about 80.
3. `dim_inactive`: a scene rect over each unfocused window, about 100 lines.
4. `hyprland-focus-grab-v1`, about 250 lines, or a launcher that doesn't need it.
5. tidewm's IPC extras: layers in `GET_OUTPUTS`, `capture` events, `send_shortcut`.
   About 200 lines.
6. Workspaces spanning every monitor: well over 1,000 lines, conflicting with every
   rebase.

Patches 1 to 3 are about 700 to 900 lines, plus a helper of about 1,500 for grants,
minimize, `keep_open` and tidewm's commands. With per-output workspaces, that fork is a
real option, at about two rebases a year. With tide's workspaces, it needs patch 6, and
tidewm, about 15,000 lines already passing all its tests, is cheaper.

[SwayFX](https://github.com/WillPower3309/swayfx), a sway fork with its own renderer,
adds real dimming but nothing else tide needs. It lagged sway releases by 2 weeks to
3.5 months, and it needs scenefx, which Debian doesn't ship. One dim patch beats
basing on it.

## Rerunning it

The branch's CI workflow runs tidewm's suite and the benchmarks against tidewm, stock
sway and sway with the helper, on Debian testing's packaged sway. Its `REPORT.md` has
the per-file results, the failure categories and how to run it locally. To try the
fork, put patches 1 to 3 on sway and point the same harness at it.
