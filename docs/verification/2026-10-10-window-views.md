# Window views experiment

The experiment lives on `experiment/window-views`, based on the pushed
`master` checkpoint `8611bec3bfabad1e4cd6c2656680daaa7de601b7`.
That checkpoint includes the pending integration-artifact cleanup. The default
overview still opens with workspace tiles.

G opens one global window board on the selected output. All windows, Apps,
Workspaces and Monitors show the same native windows in different layouts.
Shift+G cycles layouts, R changes to frozen recent-first order, and / searches
titles, app identity, workspace and monitor metadata. Group sections use up to
three columns on wide outputs; narrow outputs stack and scroll. Window previews
retain a 220 logical pixel minimum width when the viewport can accommodate it.
Selection survives regrouping and filter changes when the window remains visible.

Each card retains its weak native window identity, workspace and source output.
The display output is separate. Source captures are reused, native placement is
unchanged by grouping, and Enter/click focuses the real source window. Z fits
one window and uses the existing bounded zoom/pan model. No manual collections
or native window-group mutations are included.

## Controls and scope

See the [window board controls](../guide.md#experimental-window-board).
Escape clears search, returns to workspace tiles, then dismisses. N and
middle-click return to the workspace view before preparing an empty destination.
Window move, resize and workspace dragging use that view. Group/sort preferences
persist during one overview session; a new session starts with a flat board.
The existing fullscreen, scratchpad and covered-monitor policies apply.

## Verification

The tests use Hyprland 0.56.2 at commit
`efb50993780079460b0cbed1363e2166a2de1d9f`, with 960 × 600 outputs at scales
1, 1.25 (rotated) and 1.5. The demo uses a 1920 × 1080 primary output.
All compositor tests run in the private background display host.

- `make test`: host geometry, window-view models and existing Python build,
  packaging and runner checks.
- `make -C test asan`: host checks under AddressSanitizer and UBSan.
- `make check release-check`: exact running ABI and unchanged release metadata.
- `python3 test/integration/run.py --only window_views`: grouping and frozen
  recency, Danish search and editing, live titles, literal typing, native remote
  actions and focus, source pixels, inspection, key remapping and stale holds,
  foreground handoff, output loss, locking, unload, covered-output scope,
  fullscreen, scratchpads, workspace controls and resource release.
- Lua reload checks exercise every dispatcher mode and invalid/inactive calls.
- Existing compositor regressions cover captures, launches, input ownership,
  window and workspace dragging, resizing, switcher, layouts and transitions.
- `--only performance`: 3/12/36-window overview and board workloads, and 100
  cleanup cycles for each view. Timing ends at CPU render submission; it does
  not measure GPU completion or display presentation.

The board unload test caught a teardown callback using the global session after
its owner had been cleared. The board now retains its owning session explicitly;
unload/reload checks pass with inspection held.

The stress case also exposed mapped native windows with zero or negative
height in a crowded dwindle workspace. They now retain a selectable card and
finite command geometry, with an icon placeholder when no capture is available.

The host suite passes **842,123 checks**, including under both sanitizers.
The isolated window-view suite passes 21 checks; the zoom, transition and
activation suites pass 81, 14 and 6 checks respectively. An earlier broad run
passed 371 checks before an existing animation timing assertion failed while
capture and compilation were also running. That assertion and the complete
zoom group passed on the subsequent isolated run. The final change after those
native checks handles collapsed window cards; it is covered by the window-view
and stress suites. Exact binary hashes and cases are in the
[verification record](2026-10-10-window-views.json).

| Board workload | Input-to-render p95 | Two refresh intervals |
|---|---:|---:|
| 3 windows | 14.85 ms | 33.33 ms |
| 12 windows | 14.13 ms | 33.33 ms |
| 36 windows (stress) | 14.78 ms | 33.33 ms |

All six overview/board workloads complete, including search and focus of a
collapsed native window. One hundred board cycles and one hundred workspace
cycles release captures, text/icon textures and input ownership. These are
local measurements with disposable GTK clients, not a GPU presentation or
real-application benchmark. See the [timing record](2026-10-10-window-views-performance.json).

## Visual evidence

[All windows](../screenshots/window-views-flat.png),
[Apps](../screenshots/window-views-app.png),
[Workspaces](../screenshots/window-views-workspace.png),
[Monitors](../screenshots/window-views-monitor.png),
[Search](../screenshots/window-views-search.png) and a
[short demo](../screenshots/window-views.mp4) show real compositor output using
copied project source and disposable notes. The capture script is
[`window_views_demo.py`](../../test/integration/window_views_demo.py).

## Return to the baseline

The installed plugin was backed up before experimentation, together with its
hash and loaded-plugin metadata. On the development machine the backup is:

```
/home/simon/.local/state/hyprspace/experiments/window-views/20261010T142105Z/
```

Its executable `restore.sh` switches the clean checkout to `master` and uses
`scripts/reload.sh` to restore the saved plugin atomically. It checks the running
ABI and restores the previous binary if loading fails. The saved binary is the
original installed v1.4.0 plugin; the source checkpoint is v1.5.0. This preserves
the actual starting desktop installation as well as the committed source.
The experimental branch remains available for review or deletion afterward.
