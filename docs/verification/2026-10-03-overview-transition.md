# Overview transition recording — 2026-10-03

The workspace 10 → digit 2 sequence was recorded and inspected on the installed
desktop at 1920×1080, 60 fps, with its existing animations enabled. Only eDP-1
was connected, at scale 1.2. Same-output selection enlarged workspace 2 correctly,
but later workspace previews remained visibly superimposed over its expanding
window.

A private three-output session reproduced the reported cross-monitor failure.
The source output showed workspace 10; the destination showed workspace 1,
with workspace 2 inactive. In the original plugin, digit 2 enlarged workspace 1
and shrank/faded workspace 2, then abruptly revealed workspace 2 when the overlay
disappeared. Clicking workspace 2 enlarged it directly. Both recordings include
actual timestamps and native focus/workspace state; the private screenshots are
encoded with their measured variable cadence, rather than represented as 60 fps.

## Change

Number keys now resolve the destination across live overview views. The
destination owns the commit and closing zoom, and other views return to their
own desktops. Empty existing and fresh workspaces also resolve their owning
monitor, including native monitor rules. A remote output without an overview
keeps its native transition while the source overview restores its old desktop.

The closing destination renders above the other workspace previews. Empty
workspace selection establishes monitor and null-window focus, preserving the
owner of an empty special workspace. Populated tile commits retain their
existing window focus and cursor behavior.

## Verification

`python3 test/integration/run.py --only transitions` passes nine checks,
including loading. The tests use a landscape output and a rotated output at
fractional scale. They verify destination growth rather than growth of its old
workspace, closure of all views, an actual opaque marker covering overlapping
previews, geometry near the desktop endpoint and marker pixels immediately
after handoff. They also cover empty tiles, fresh bound workspaces, an uncovered
output, an already-active destination with native back-and-forth enabled, and
empty special workspace ownership. Results: `/tmp/hs-i.1sd9hvwi`.

`make test` passes 332,504 host assertions, 69 build/reload checks and the helper,
installer, command, runner and artifact tests. Formatting, whitespace and the
exact running compositor ABI checks pass.

The full private `--only all` suite also passes on the final binary, including
the new transition checks and isolated Lua/dispatcher lifecycle sessions.
Results: `/tmp/hs-i.tt0smddm`; a retained copy is
`build/verification/transition-review/full-suite-results.json`.

The corrected plugin is installed and loaded:
`393fdd86a35ed63e17f2379936d8f039131d465d25e50d274e382bb7e533031a`.
The final physical recording repeats digit 2 and digit 0 and restores workspace,
focus, pointer, options and visibility. Results: `/tmp/hs-physical.yxcak1pr`.

Video, contact sheets, timestamps and result records are retained locally under
`build/verification/transition-review/`. Cross-monitor video uses disposable
clients on private outputs; it is not a physical multi-monitor recording.
The aligned comparison is `comparison/before-after-number2.mp4`; its original
and corrected captures average 16.27 and 17.03 fps. Presentation timestamps
follow the original samples without interpolating motion. The final physical
clip is `workspace-10-to-2-after.mp4`, recorded at 60 fps.
