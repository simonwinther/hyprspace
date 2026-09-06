# Overview resize bounds, September 6, 2026

Overview resizing now constrains floating windows to the source monitor's usable
area and clips the preview to the workspace's current animated boundary. Pointer
release over a gap or another output completes the resize in place. Moves still
require a valid destination tile, and tiled resize gestures retain native layout
handling.

The background reproduction grew a 320 by 240 floating window to 3093 by 331 and
transferred it from workspace 11 to workspace 13. A tiled window could display an
oversized preview even when the native layout refused that geometry. The new
checks cover both provisional rendering and committed placement.

| Check | Result |
| --- | --- |
| Core background interaction suite | 133 passed |
| Native drag comparisons within that run | All 27 layout/output pairs passed |
| Final background resize suite | 32 passed |
| Host assertions | 329,103 passed |
| Host AddressSanitizer and UndefinedBehaviorSanitizer | 329,103 passed |
| Build and reload fixtures | 69 passed, with simulated compositor calls |
| Launch helper tests | 7 passed |
| Background runner isolation tests | 10 passed |
| Exact ABI, release metadata, formatting and diff checks | Passed |

Resize coverage includes all four corners on three outputs in dwindle, scrolling
and master, with portrait rotation, fractional scale, negative offsets and reserved
space on every edge. Additional cases exercise forced corners, aspect ratios,
minimum/maximum sizes, oversized recovery, fullscreen/maximized floats, floating
groups, native snapping, drag thresholds, panel pointer capture, cancellation and
immediate modifier release.

The 133-check core run completed in `/tmp/hs-i.wj_7_ejk` with binary SHA-256
`accffeafe7a0ee2e0b0e1a16c5746128a0e528867ce0b2e9d6962c4993bf2332`.
An additional animation check then found 7,919 bright client pixels outside the
settled workspace when a resize started during opening. The final rendering change
follows the current workspace clip; the screenshot assertion now finds zero such
pixels. The final 32-check resize run completed in `/tmp/hs-i.z9zbi_68` with binary
SHA-256 `5ab636ca6b649c805cbcedfdd138dccf0586c05d19cba754c84ec0bf7d69eda1`.
The native placement and input code did not change after the core run.

Logs, result lists, source hashes, the animation screenshot and the previous
installed binary are retained in `build/verification/resize-bounds-2026-09-06/`.
The tested binary was initially installed by atomic rename without reloading the
running desktop plugin. At the user's request, `make reload` activated it on
September 6 at 02:18 UTC. The compositor maps the new installed inode, the old
mapping is gone, and the private status interface responds. Hyprland reports one
loaded Hyprspace plugin and no configuration errors. The activation record is
retained beside the test logs. Tests used no visible windows or physical input
injection, and Omarchy source files were left untouched.

This change applies to overview gestures. Ordinary desktop resizing remains under
Hyprland's control. If an application's minimum size cannot fit the work area,
the floating resize is cancelled rather than violating that minimum. Physical
hardware tests and application-specific game testing were not repeated.
