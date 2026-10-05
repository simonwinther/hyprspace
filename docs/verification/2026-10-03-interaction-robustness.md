# Interaction robustness verification — 2026-10-03

The implemented changes passed the full private suite and checks on the installed desktop. The plugin loaded at completion matched the tested binary: `81235b7558998137e6cef8d430748b724a29d2910ac6e46aad32ed57806fc037`. A subsequent [recorded transition fix](2026-10-03-overview-transition.md) supersedes that installed build and includes another full private suite run.

## Behavior verified

- Hovered Super+W remains correctly targeted across repeated real Lua reloads and restores native dispatchers on unload.
- Correlated launches freeze workspace and cursor placement. Floating decorated bounds stay in the usable area; oversized windows preserve size with accessible leading edges. Existing windows and explicit rules retain native behavior.
- Captured live workspace objects survive renames and monitor transfers. Destroyed/recreated workspaces cannot inherit a stale launch token. Delayed concurrent launches retain their own points through overview dismissal.
- Native drag thresholds use mapped logical movement with strict `>` comparison. The 120 exact cases cover move/resize directions and corners, including immediate Super release after a nonzero threshold.
- Active gestures retain panel pointer ownership and consume wheel input. Released resize pictures remain visible within one pixel of the held picture until native commit. Raw panel hover cannot alter pending geometry. Dismissal, window loss and output removal cancel pending work.
- Cursor shapes restore prior overrides and preserve a newer external override.
- A 5120×2880 source produces a 3861×2172 preview (33,544,368 RGBA bytes), within 32 MiB. Four corner markers, a far-offset child subsurface, nonempty source clipping and continuing callbacks/repaints pass on ordinary and rotated fractional outputs. Logical desktop geometry stays unchanged.
- Opening/closing during zoom and workspace panning preserves same-tick cell geometry within one logical pixel.

## Automated checks

`make test`: 332,504 C++ assertions, plus build, helper, installer, physical command, runner and artifact tests. Host AddressSanitizer/UndefinedBehaviorSanitizer checks pass. Release metadata, 31 release tooling tests, formatting and the exact compositor ABI check pass.

The full private `--only all --companions build/companions/bin` run passes 238 checks, plus its isolated Lua and dispatcher lifecycle sessions. It includes all 27 output/layout pairs across dwindle, scrolling and master, three output scales, portrait transformation, negative coordinates, fullscreen/maximized/groups, lock, foreground layers, reload/unload and actual Walker keyboard/mouse activation. Artifact directory: `/tmp/hs-i.gq1kk04t`.

## Responsiveness

AMD Ryzen 5 PRO 4650U / Radeon Vega (Renoir), Hyprland 0.56.2 `efb50993780079460b0cbed1363e2166a2de1d9f`. Three private 60 Hz outputs. Measurements end at compositor `RENDER_POST`: CPU render submission, before output commit. GPU completion and display presentation are excluded.

| Windows | Input p95 ms | Preparation p95 ms | Capture p95 ms | Render submission p95 ms |
|---|---:|---:|---:|---:|
| 3 | 16.75 | 0.27 | 0.19 | 2.36 |
| 12 | 17.31 | 0.55 | 0.40 | 2.65 |
| 36 | 16.70 | 0.93 | 0.74 | 2.03 |

The 3/12-window ordinary workloads pass the two-refresh-interval budget (~33.3 ms). The 36-window case is recorded separately as stress. All 100 open/close cycles release capture/texture bytes, input ownership and pending placement. Diagnostics are opt-in, bounded to 64 samples and clear when disabled. Artifact directory: `/tmp/hs-i.yh0vqeqg`.

## Installed desktop

The physical suite passes nine checks on the available eDP-1 output (1920×1080, scale 1.2, ~60 Hz) using the actual Lua configuration: three layouts, immediate-release resize, scrolling controls, hardware/software cursor visibility, hovered Super+W after three reloads, and restoration. Virtual shortcut testing temporarily uses symbol matching for its own keymap; the original setting is restored. Print/Escape use physical evdev codes through the persistent test keyboard. Artifact directory: `/tmp/hs-physical.m4f1qk8n`.

Installed launcher checks pass four checks: delayed desktop-application placement through Walker/Elephant, notification layering, Print/Escape selector handoff and desktop restoration. Artifact directory: `/tmp/hs-physical.lzzr7vzl`.

Patched Walker 2.17.0 and Elephant 2.22.0 are active from a verified personal versioned installation. All 12 previously installed providers were built with the same Go 1.25.14 toolchain and are loaded. Existing launcher arguments, configurations, XDG paths and the background-grid menu route are preserved. Packaged binaries and Omarchy-managed source files remain available. Rollback manifest: `/home/simon/.local/share/hyprspace/companions/rollbacks/5927441a0176dd9a-1791042447675956676/manifest.json`.

Only one physical output was available. Three-output behavior is verified on private outputs; physical three-monitor hotplug and existing-process Firefox/Discord checks remain separate release gates. This work does not publish a release.

Reproducible commands are in [interactive verification](../interactive.md#verification) and the [companion guide](../../companion/README.md). Machine-readable results are retained in `build/verification/interaction-robustness/`.
