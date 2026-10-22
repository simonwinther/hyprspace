# Normal-size preview filtering comparison

The independent visual choice is **`sharp`**: area minification followed by a restrained, neighborhood-clamped sharpening pass. It preserves more complete character strokes than the previous single bilinear lookup and gives slightly better definition than area minification alone. It does not make arbitrarily small text readable.

The four modes were compared using the same seven live windows, five occupied workspace cards and native 1920 × 1080 output. The fixture uses real Ghostty/Neovim editing windows and local, static Firefox articles, with dark and light content. No zoom, card enlargement, capture-resolution change, post-capture sharpening or screenshot resampling was used to create the comparisons. Detail images only crop the captured output pixels.

## Visual result

| Mode | Assessment at the normal overview size |
| --- | --- |
| `linear` | Dense terminal text has uneven, broken strokes and visible speckling. Larger headings remain recognizable. |
| `linear-sharp` | Adding sharpening to bilinear minification strengthens the aliasing along with the useful detail. It does not recover missing strokes. |
| `area` | Glyph strokes are more coherent, small browser text is easier to distinguish, and dense text loses much of its speckled appearance. A little soft. |
| **`sharp`** | Best balance in these fixtures: the coherent strokes from area minification with a modest increase in definition. No objectionable halos were visible in the native-size light/dark comparisons. |

The improvement is visible at the normal card size, particularly in the light code window and small article text. Review included both full overview frames and unscaled crops at output scales 1, 1.2 and 1.5. The conclusion was consistent across these scales; it is a visual judgment from these fixtures, not a claim that every possible application benefits equally.

Durable current-scale screenshots:

- Full overview: [previous linear](../screenshots/preview-clarity/linear.png), [area](../screenshots/preview-clarity/area.png), [chosen sharp](../screenshots/preview-clarity/sharp.png), [linear plus sharpening](../screenshots/preview-clarity/linear-sharp.png).
- Native-pixel code detail: [linear](../screenshots/preview-clarity/linear-code-detail.png), [area](../screenshots/preview-clarity/area-code-detail.png), [sharp](../screenshots/preview-clarity/sharp-code-detail.png), [linear plus sharpening](../screenshots/preview-clarity/linear-sharp-code-detail.png).

The complete local matrix, native desktop reference captures and an interactive A/B page are in [`build/preview-clarity-2026-10-22`](../../build/preview-clarity-2026-10-22/comparison.html). The page compensates for the browser's device pixel ratio so one image pixel occupies one device pixel. It allows switching variants without changing scroll position.

## Validation and measured cost

Frozen native plugin SHA256:

```text
d07bf6f547667cbb530dc6c3f2efd6a17070aef4b6e859e16bc1b43508146498
```

Private compositor artifacts: `/tmp/hs-i.ka9sah62`. The run completed successfully and its processes were closed. The host desktop was neither loaded with the comparison build nor captured by this script.

All 12 filter/scale cases passed:

- Every variant retained identical workspace and window preview geometry at its given scale.
- All output screenshots were exactly 1920 × 1080 physical pixels. Fractional output captures used `grim -s <monitor scale>` to avoid a second downsampling operation.
- Native capture downsampling and filter failures remained zero; non-linear variants proved that their filter targets were allocated, rather than silently falling back.
- Capture and filter allocations stayed below the shared 256 MiB resource budget.
- Every close released capture, filter and text/icon texture bytes, input ownership and cursor ownership.

Current scale 1.2, 60 input steps per mode:

| Mode | Input to render p95 | Render submission p95 | Frame interval p95 | Extra filter bytes |
| --- | ---: | ---: | ---: | ---: |
| `linear` | 14.20 ms | 2.98 ms | 18.41 ms | 0 |
| `area` | 13.23 ms | 2.94 ms | 18.62 ms | 3,465,784 |
| **`sharp`** | **14.89 ms** | **3.25 ms** | **18.59 ms** | **6,931,568** |
| `linear-sharp` | 14.61 ms | 2.66 ms | 18.33 ms | 6,931,568 |

These diagnostics end at compositor `RENDER_POST`. They measure CPU submission and frame scheduling, **not GPU completion or physical presentation**. Small differences in a single run should not be treated as exact filter cost. In this workload all modes remained within two 60 Hz refresh intervals. Chosen `sharp` used 46,991,680 total capture/filter bytes, including approximately 6.61 MiB of filter targets.

All four modes also passed four real animated open/close cycles each. The chosen mode recorded 256 primary-output samples, 19.02 ms frame-interval p95 and 56 filter target allocations: exactly two targets per window per cycle, rather than one allocation per changing animation frame. It bypassed filtering on 1,198 animated preview draws. Desktop geometry and focus were restored exactly after each close; all resources returned to zero. The full [measurement record](../screenshots/preview-clarity/measurements.json) includes the other scales and animated modes.

Reproduce with the current matching build inventory:

```sh
python3 test/integration/preview_clarity_demo.py \
  --output build/preview-clarity-2026-10-22 \
  --scales 1.2 1 1.5
```

The harness always creates an isolated compositor and private application profiles. It never falls back to the desktop session.

## GPU safety follow-up

The following independent checks used plugin SHA256 `184822d1fe209b817f7ab47410de505dbb4c98164a65ca2f72c9b87edd319b34` and native fixture SHA256 `b98540340eb2e1b0154a05cc25d5144e73d9aef66cae769c697ecfa27464262d`. These identify the exact tested build; subsequent selector layout fixes do not change the graphics path.

| Private suite | Passed checks, including ABI load | Local artifacts |
| --- | ---: | --- |
| Capture | 8 | `/tmp/hs-i.il38iu6w` |
| Resources | 6 | `/tmp/hs-i.yk86purd` |
| Targeted filter safety | 9 | `/tmp/hs-i.wgmeey9s` |

Capture checks verified four source colour corners, a distant child subsurface, native visible-region clipping and restoration, frame callbacks and teardown on ordinary and rotated outputs at scale 1.25. Source capture probes reported no GL errors. Resource checks exercised bounded cache churn, allocation failure/backoff/recovery, retained frame textures and unloading open overlays.

The targeted probe compared actual native output screenshots of a premultiplied RGBA window with 0.55 alpha. It covered ordinary and portrait/fractional outputs with blur disabled, live blur and cached blur. All six comparisons had **zero channel difference** between `linear` and `sharp` inside the four flat colour regions. Quadrants stayed upright, clipping and desktop input geometry were preserved, and filter failures remained zero. Blur was exercised: the ordinary red-region sample changed from `[146,20,23]` with blur off to `[149,23,26]` with live blur and `[150,24,24]` with cached blur; each configuration still matched the native linear reference exactly.

Unmodified screenshots are preserved as [ordinary cached blur](../screenshots/preview-clarity/gpu-ordinary-cached-sharp.png) and [portrait/fractional cached blur](../screenshots/preview-clarity/gpu-portrait-fractional-cached-sharp.png). They were copied byte-for-byte from the private run. The [measurement record](../screenshots/preview-clarity/gpu-safety-measurements.json) preserves the generation, six colour comparisons and full-size bypass counters.

An enlarged stable preview bypassed filtering: during a 250 ms observation, submitted filter passes stayed at `218` and framebuffer allocations stayed at `16`. Releasing zoom resumed normal-size grid filtering. Every close released all capture/filter/UI texture bytes; unloading with populated filter buffers and reloading left zero filter entries/bytes and released keyboard/cursor ownership. All three private sessions were closed, and their compositor logs contained no GL error or crash markers.

Reproduce the targeted probe with a matching current build inventory:

```sh
python3 test/integration/run.py --only capture
python3 test/integration/run.py --only resources
python3 test/integration/preview_filter_safety.py
```

The targeted probe creates a private compositor and a disposable RGBA GTK window. It does not operate on the desktop session. It compares flat regions to verify sampling/compositing correctness; the earlier real-application A/B matrix provides the visual sharpness assessment.

## Filtering references

The [Khronos sampler documentation](https://wikis.khronos.org/opengl/Sampler_Object) describes minification and mipmap sampling. [AMD's CAS documentation](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/contrast-adaptive-sharpening/) provides useful context for restrained sharpening and artifact avoidance. The implementation tested here is a bounded 16-sample area approximation and a five-sample locally clamped sharpening pass; it does not import or claim to implement CAS.
