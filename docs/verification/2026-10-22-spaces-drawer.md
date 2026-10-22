# Spaces drawer verification — 2026-10-22

This record covers the C++ on-demand Spaces drawer and theme revision. The
final installed plugin has SHA-256
`b4d8fc78e3e310cc52519adec3c4c2c0edb55b0d58afc0593e06bd366b1b7e20`.
Its production build, focused product visual checks, ABI-guarded deployment,
live preservation checks, and final focused performance checks passed. It adds
only the context-name measurement correction to the `1685cb6e` structural
revision; broader functional and native
border evidence below remains explicitly tied to that preceding hash.
The existing Super+A overview remains the entry point. The drawer starts hidden,
opens inside the overview, and closes after choosing a space while the overview
stays open. There is no additional desktop entry or global shortcut.

The private compositor is Hyprland 0.56.2, commit
`efb50993780079460b0cbed1363e2166a2de1d9f`, local ABI
`efb50993780079460b0cbed1363e2166a2de1d9f_aq_0.15_hu_0.14_hg_0.5_hc_0.1_hlg_0.6`.
Run directories retain `results.json` and `generation.json` with the exact
plugin and fixture identities. These checks do not extend the supported
compositor or distribution matrix.

Functional and performance fixtures use three virtual outputs with mixed
scales and a rotated output. The wide visual fixture uses 1600 × 900 logical
pixels at scale 1.2 with a 56-pixel reserved panel. Live deployment was checked
on the actual desktop's one physical output; this is not physical multi-monitor
or hotplug validation.

## Final context-name fit correction

The final build uses a bounded 32-entry CPU measurement cache keyed by name,
font, and actual output scale. Measurement at the physical font size plus a
small rounding allowance keeps Default fully visible at Sans 12 / scale 1.2.
Long names still ellipsize at the 280-pixel trigger cap. This changes no input,
selection, namespace, palette, or shared border-rendering behavior.

The native-scale dark/light visual fixture passed two checks at
`/tmp/hs-i.bevid8yn` with the final hash. Actual Default label crops passed OCR
and independent visual review in both themes. The long-name capture preserves
the chevron, all four real input edges are continuous in both themes, and the
480-pixel overview/editor fit. Relevant images include
`spaces-context-label-dark.png`, `spaces-context-label-light.png`,
`spaces-long-context-light.png`, `spaces-focused-input-dark.png`,
`spaces-focused-input-light.png`, `spaces-drawer-wide.png`,
`spaces-drawer-light.png`, `spaces-collapsed-compact.png`, and
`spaces-appearance-compact.png`.

## Final live deployment

The final `b4d8fc78` plugin is loaded through the ABI-guarded reload path; the
installed and built hashes match. Reload preserved all 13 windows with zero
workspace-assignment changes. Live validation confirmed the existing empty
space has no occupied workspaces or leaked bar occupancy, then restored the
original space, local output selections, and focus. Overlay input ownership
was released and configuration errors were clear.

The pre-deployment backup and actual host evidence are retained at
`/home/simon/.local/state/hyprspace/context-chooser-backup-20261022-210641`:
`reload-preservation.json`, `live-validation.json`,
`live-spaces-populated.png`, `live-spaces-drawer.png`, and
`live-spaces-empty.png`. Independent inspection of all three host captures
confirmed the full Default label, compact two-space chooser, and empty state
fit the real desktop. These final captures supersede the earlier live images.

## Final focused performance

One focused run on the final `b4d8fc78` hash passed four checks at
`/tmp/hs-i.rf7w465o`, using the unchanged 33.3333 ms warm-switch p95 gate.
Across twenty warm switches, all-output p95 was 26.4817 ms and the maximum was
30.6194 ms. Per-output p95 values were 18.0441, 21.6736, and 26.4817 ms.

With twelve active windows held fixed, adding four inactive spaces containing
24 windows kept capture memory at 5,729,188 bytes and preserved every output's
capture pixel count. Steady sampling rebuilt membership zero times. Render
p95 changed from 1.7822 to 1.9636 ms; preparation p95 changed from 0.6289 to
0.6886 ms. The full 100-switch cleanup exercise retained bounded GPU resources
and released GPU and input ownership after closing.

Evidence: `build/spaces-border-performance-final.log`,
`build/spaces-border-performance-summary.json`, and the run directory above.
This focused four-check run does not claim a new full 13-check performance
suite or a complete all-groups/zoom-driver run.

## Structural revision `1685cb6e`

This section used plugin SHA-256
`1685cb6e00ae05c15beba7ddf5894514a609529643fe653ee26f222cfe4a7d6c`.

| Check | Result | Evidence |
|---|---|---|
| Production plugin build | Passed | Structural-revision hash above |
| Full host suite | 844,463 checks, zero failures | `build/spaces-border-host.log` |
| Shared rendering geometry | 102 checks, included in host total | `test/test_render_geometry.hpp` |
| Theme contrast and icon groups | 673 and 321 checks, included in host total | Existing focused host groups |
| Installed-native region coverage probe | Records truncated fractional coverage before endpoint snapping | `build/border-diagnosis/native-region-probe.log` |
| Native GPU border comparison | 6 passed | `/tmp/hs-i.e07_1p46` |
| Space/bar IPC | 9 passed | `/tmp/hs-i.ugnjwr_a` |
| Configuration | 10 passed | `/tmp/hs-i.k16i279e` |
| Selector with Sans 12 | 11 passed | `/tmp/hs-i.mviln8ey` |
| Native-scale dark/light visual fixture | 2 passed | `/tmp/hs-i.k_z8wo8v` |
| Lua bindings | 4 passed | `/tmp/hs-i.u10cwpny` |
| Camera pan | 5 passed | `/tmp/hs-i.7hh9mi7g` |
| Held-zoom edges | 10 passed | `/tmp/hs-i.t1whbke4` |

The current context icon, neutral name, and chevron now form one trigger.
The chooser uses single-line 44-pixel context rows with neutral focus fill,
window counts or an Empty label, and a separate current-space checkmark.
New space and Manage spaces are quiet vertical actions below one separator.
The repeated heading, extra close control, row outlines, and permanent keyboard
manual are removed. The 432-pixel editor no longer has an ornamental preview.

Shared rendering helpers align filled and outlined geometry to the same snapped
physical endpoints and preserve logical border-width semantics. The native
region probe is diagnostic evidence. The separate native GPU comparison
reproduced the missing right/bottom straight edges with raw fractional geometry
and rendered all four complete edges with snapped geometry at scales 1, 1.2,
1.25, and 1.5, plus a rotated 1.25 output.

At scale 1.2, the raw straight-edge samples were top 193/193, right 0/17,
bottom 0/193, and left 17/17. The corrected samples were top 195/195, right
19/19, bottom 195/195, and left 19/19. An independent pixel recount from all
five actual PNGs confirmed the recorded coverage. The A/B images were also
visually inspected. Product Sans 12 input crops in both themes also show four
continuous edges; `spaces-focused-input-dark.png` and
`spaces-focused-input-light.png` under `/tmp/hs-i.k_z8wo8v` retain their font,
scale, and input-box metadata in adjacent JSON files. Chooser and compact-form
review found the intended simplified structure, but the Default header label
was prematurely ellipsized at scale 1.2. The final measurement correction and
its actual capture proof are recorded above. Palette derivation is cached until
its configuration inputs change.

## Earlier candidate `35217adb`: passed checks

All checks in this section used plugin SHA-256
`35217adb3dedc629b83b4ed93739c00c5b9a3f2d86f4f3895d6f381151024b81`.

| Check | Result | Evidence |
|---|---|---|
| Full host suite | 844,361 checks, zero failures | `build/spaces-style-host-final.log` |
| Theme contrast group | 673 checks, included in host total | `test/test_space_style.hpp` |
| Icon group | 321 checks, included in host total | `test/test_space_icons.hpp` |
| Selector integration | 11 passed | `/tmp/hs-i.07iqza4i` |
| Wide visual fixture | 2 passed | `/tmp/hs-i.wnr6g04d` |
| Space/bar IPC | 9 passed | `/tmp/hs-i.yoz5pqce` |
| Lua configuration and bindings | 4 passed | `/tmp/hs-i.1zqjik20` |
| Camera pan | 5 passed | `/tmp/hs-i.q1xa_t1d` |
| Held-zoom edge browsing | 10 passed | `/tmp/hs-i.sqlxc291` |
| Configuration | 10 passed | `/tmp/hs-i.p8v_66uu` |

Integration counts include startup checks where the runner records them. The
host suite tests pure model, rendering, and style helpers; it does not
instrument the plugin inside the compositor.

The selector checks cover hidden/open drawer geometry, inspection transitions,
captured window gestures, Unicode names, persistent icon/color edits, and F2 /
Delete acting on the highlighted inactive space. Arrow keys move the highlight
without changing the canvas; Enter switches all outputs and returns to the
overview. Moving windows or complete workspaces, populated-space removal with
a transfer preview, and all controls fitting a 480-pixel display also passed.
An earlier compact-output resize acknowledgement timed out; the unchanged
plugin passed the complete 11-check rerun recorded above.

IPC checks cover active-space-only occupancy, local workspace selection,
idempotent current-slot clicks, stale/malformed request rejection, and native
coalesced change events. Steady frames and status queries emit no change
events. The Lua fixture exercises the existing Super+A binding and the drawer
workflow without adding another entry point.

## Earlier candidate: visual evidence

The wide fixture uses 1600 × 900 logical pixels at scale 1.2 with a real
56-pixel reserved panel. Actual compositor captures were reviewed for the
collapsed overview, drawer, editor, and empty state in dark and light palettes.
The School, Work, and Hobby spaces are private fixture data.

The private captures initially showed the compact drawer, neutral names,
identity icons, and editor fitting the available space. Actual host review
subsequently exposed fragmented borders and excessive chooser chrome. These
captures do not establish acceptance of the current structural revision.

Relevant captures under `/tmp/hs-i.wnr6g04d`:

- `spaces-collapsed-wide.png`, `spaces-drawer-wide.png`,
  `spaces-appearance-wide.png`, `spaces-empty-wide.png`
- `spaces-collapsed-light.png`, `spaces-drawer-light.png`,
  `spaces-appearance-light.png`

Compact captures under `/tmp/hs-i.07iqza4i`:
`spaces-compact-appearance.png` and `spaces-compact-manage.png`.

`spaces:theme = auto` derives brightness from the configured overview colors;
it does not query operating-system appearance. `dark` and `light` override the
control surfaces. Complete light fixtures also set the existing overview
background, title, label, and tile colors. Standard text targets 7:1 contrast,
secondary text has a 4.5:1 floor, and identity icons/field focus rings target
3:1. See [design references and decisions](../design/spaces.md).

## Installed slider and historical evidence

The previously deployed Rust `workspaces-slider` remains the existing desktop
surface. Its binary SHA-256 is
`ad595e8e97f4fe806456d0884884ba089f1b0e30dc0aa2f1b25cdf3eb4cb2260`.
Its six Rust tests and seven real compositor integration checks passed at
`/tmp/hs-i.yfp66x9q`. Those checks concern the slider binary and its earlier
plugin fixture, not a new slider build for this drawer revision. It projects
the active namespace and updates from native events.

The earlier [Spaces verification record](2026-10-22-spaces.md) retains prior
candidate results. The earlier 13-check performance evidence, including the
`21dc2986078b11aae8fe55e14f64cfae274beb08d526f3a2bcb3e73b2aeb07cb`
revision, is historical and does not certify this drawer candidate. No single
complete `--only all` or full zoom-driver run is claimed for this revision.

## Earlier candidate: live deployment and configuration

The review candidate was installed and loaded through the ABI-guarded reload
path; its installed SHA-256 matched the hash above. Asset and license installation
also passed. Live validation preserved all 13 existing windows, confirmed zero
occupancy and an empty bar projection in an existing empty space, and restored
the original space, per-output selections, and focus. Configuration errors were
clear and overlay input ownership was released.

The fresh pre-deployment backup and live evidence are retained at
`/home/simon/.local/state/hyprspace/drawer-backup-20261022-204801`:
`live-validation.json`, `live-spaces-populated.png`, `live-spaces-drawer.png`,
and `live-spaces-empty.png`. These are actual host captures, distinct from the
private fixture captures above. Host review found an ellipsized close glyph
with the inherited Sans 12 font and fragmented outlined edges. The current
revision removes the redundant close control and changes shared rendering
geometry; these earlier captures cannot certify that follow-up.

The configuration suite verifies supported `auto`, `dark`, and `light` values
and readback, ending with `auto` restored. It does not claim that the legacy
native keyword path rejects every unsupported string: that path can retain an
unknown value, which the palette safely treats as automatic mode.

## Historical performance misses and validation scope

The earlier `35217adb` candidate's warm-switch p95 missed the 33.333 ms performance gate
twice: 33.4747 ms at `/tmp/hs-i.g51wi839` and 34.7538 ms at
`/tmp/hs-i.g3bvqhla`. Its performance is not recorded as a pass.

Package/hyprpm installation and physical multi-monitor release checks are not
covered by these private compositor runs. The current drawer revision's
requested build, visual, functional, deployment, and focused performance work
is complete.
