# History removal and preview brightness

Plugin SHA-256:
`7beaea21b8f60ba39c36f52fcbbaf2c059888a7d8efc7fbd230d75f16cb93a2c`.
Matching native fixture:
`ba2dc5b3f430611c47de419f125199fad00db2649d980ab3f1ede7eb6e8f5153`.

The history drawer, `Space h`, Ctrl+O/I visit navigation, Lua/dispatcher APIs,
recording hooks and persistence model are removed. No global replacement was
added. Existing version-1 space files still load; optional legacy history
metadata is ignored without affecting spaces, workspace ownership, appearance,
order, pins or monitor selections. Native Alt+Tab recency remains available.

The previous area filter averaged encoded RGB values, losing light in narrow
bright strokes. Filtering now decodes each texel before interpolation, averages
in linear light with premultiplied alpha, and encodes the result for the native
compositor. Sharpening preserves center coverage and uses unassociated color
to avoid alpha fringes. Transfer-function changes invalidate the cache;
unsupported transfer functions use the native texture path. Buffer allocation
counts and limits are unchanged.

Common shrink ratios integrate each overlapping source pixel once, with exact
coverage weights. This avoids repeatedly decoding the same texel in bilinear
taps. Larger footprints retain a bounded sample count. Independent scalar
checks covered 209 footprint/coverage cases.

Workspace selection now changes the border without adding the previous 10%
fade to unselected windows. Configured window opacity remains applied once.

## Native image comparison

The same source, card geometry and selected target were used before and after.
Each source includes flat grays, narrow strokes, a checker pattern and dense
text. The enlarged comparison bypasses filtering at nearly source resolution.

| Configuration | Old text light | Corrected text light | Corrected / enlarged |
| --- | ---: | ---: | ---: |
| Opaque, no blur | 0.03775 | 0.06815 | 1.012 |
| 0.85 opacity, no blur | 0.02955 | 0.05148 | 1.030 |
| 0.85 opacity, live blur | 0.03012 | 0.05226 | 1.033 |
| 0.85 opacity, cached blur | 0.03050 | 0.05277 | 1.034 |

These values are mean linear luminance of the text patch, not a uniform image
brightness adjustment. Flat patches are unchanged. Changing the selected
workspace produces exactly zero channel-mean difference for the same window.
The regression also checks strokes/checkers against native light and allows
for crop/rasterization differences between native, minified and enlarged text.

Original native screenshots: [before](../screenshots/preview-brightness/before.png),
[after](../screenshots/preview-brightness/after.png),
[enlarged source](../screenshots/preview-brightness/after-source-bypass.png).
Exact image hashes, measurements and crop bounds are in
[measurements.json](../screenshots/preview-brightness/measurements.json).

## Validation

- `make test`: 840,602 C++ checks, 2 Rust checks, 69 build/reload checks and all
  included Python suites passed; rendering boundaries passed all 6 checks.
- Brightness: 7 native checks, `/tmp/hs-i.1e_qclle`.
- Shared input: 9 native checks, `/tmp/hs-i.k_eojusq`.
- GL state, RGBA colors, fractional/rotated outputs, blur, enlarged bypass and
  cleanup: 9 checks, `/tmp/hs-i.1ro4x0cp`.
- Explicit window/workspace transfers: 11 checks on
  `df084893df01d929cc024857ca0f45b2be4863774178028f0c3b74ad909554fd`,
  `/tmp/hs-i.ot0pomlc`. Only the filter's internal footprint integration changed
  afterward; transfer code and view geometry are identical.
- Removed Lua methods across startup, reloads and recovery: 36 checks on
  `e5e35599b44beba6a6e4761fc2a0582b63c195f32fb9c64efc8d1afae76d457a`,
  `/tmp/hs-i.pv5uuopl`. Subsequent source changes only affect preview sampling
  and opacity; final shared-input checks also verify the removed commands.

The native image test caught an invalid promotion of a compositor-owned weak
image-description handle before installation. The filter now borrows that
checked handle on the compositor thread. All final native checks above use the
corrected build or their explicitly identified precursor. Logs/results are under
`build/history-brightness-*`, `build/optimized-preview-native-*`,
`build/final-preview-native-*` and `build/brightness-native-results.json`.

## Performance

An isolated ABBA comparison used the same seven live Ghostty/Neovim and Firefox
windows, five workspace cards and exact output/window geometry across 12 trials
and 2,328 primary-output render callbacks. Compilers and other test compositors
were stopped. The old baseline was `e5e35599…`; the candidate was `7beaea21…`.

| Metric | Old filter | Corrected filter |
| --- | ---: | ---: |
| Sharp render submission p95 | 3.0416 ms | 3.0702 ms |
| Area render submission p95 | 3.2664 ms | 3.3586 ms |
| Sharp input step p95 | 14.8823 ms | 14.6151 ms |
| Sharp callback interval p95 | 17.6856 ms | 17.9691 ms |

Sharp callback cadence remained about 60 Hz. Both versions used 6,931,568 filter
bytes, without steady-state allocations, failures, omissions or capture
downsampling; teardown released all resources. These are CPU submission and
render-callback measurements, not GPU-completion or physical-presentation times.
The first, redundant-sampling correction cost about 13 ms at p95 and was not
installed; exact integration removed that regression without losing brightness.

Full paired results, immutable generations and raw frames:
`build/preview-gamma-perf-optimized/performance.json`.
The intermediate is retained separately under
`build/preview-gamma-perf-final-controlled/`.

## Live installation

The ABI-checked atomic reload installed the exact tested binary. Live checks
verified that history APIs/status/leader entries are absent, Ctrl+O/I is inert,
and both explicit move scopes open and cancel in the grid and window view.
No transfer was submitted. All seven current windows, three spaces, logical
workspace identities/order, placements, monitor slots and focus were preserved.
One inactive empty native workspace was reaped on reload; its logical record
and metadata remain intact. All capture/filter/UI bytes and input ownership
were released on close, with zero filter failures or configuration errors.

Fresh backup and private live screenshots/status:
`/home/simon/.local/state/hyprspace/history-brightness-backup-20261023-000123`.
Final live result: `build/history-brightness-live-result.json`.
All private test compositors were closed.
