# Fullscreen switcher verification

Checked on 2026-09-08 against Hyprland 0.56.2, commit
`efb50993780079460b0cbed1363e2166a2de1d9f`.

An opaque fullscreen window can become Hyprland's solitary render candidate.
That path skips the workspace stages where hyprspace draws its overlays.
The regression test reproduced an invisible Alt+Tab panel on the previous
plugin while tiled and maximized panels remained visible. Dismissing the
startup notification is necessary to exercise this optimization.

The plugin clears that candidate before rendering checks on outputs with a
visible overlay, including its closing animation. The normal pass draws the
switcher above windows and below foreground layers and cursors. Hyprland
rechecks the candidate on the next frame after dismissal.

The background switcher suite passed 12 checks with both the Arch build and
the Nix package. It checks panel pixels, forward and reverse Alt+Tab, further
selection, Escape, Alt release, fullscreen state, rotated and scaled outputs,
independent fullscreen outputs and animated closing. The foreground suite
passed 8 checks and the input/visibility audit passed 10 checks.

The Arch container build, host tests, 329103 sanitizer checks and 25 release
tests passed. The Nix package build, helper paths, compiler agreement, embedded
ABI and unsupported-input rejection passed. Its private compositor required
one fresh startup after an initial exit before the plugin loaded.

The Nix package is
`/nix/store/vq1jy6s4bym6xa6sgf7a9hi3pi78qrl0-hyprspace-1.0.0`, with compositor
`/nix/store/sirqxk22z7x7yh1nz4pimw0akw4apgqw-hyprland-0.56.2+date=2026-08-05_efb5099`.
Logs and result JSON are retained in
`build/verification/fullscreen-2026-09-08/`.

All desktop checks used private display servers. These checks do not exercise
physical direct scanout, tearing, or the separate companion compatibility gate.
