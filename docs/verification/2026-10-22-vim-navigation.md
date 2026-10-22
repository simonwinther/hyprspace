# Vim navigation and persistent places

## Follow-up: implicit navigation and search

The subsequent UI revision removes the persistent Normal/Insert corner badge
and mode names from search/editor footers. The corner guide appears only for
a pending Space or g sequence. Keyboard modes and routing remain unchanged.
The Space guide ends 16 logical pixels above the usable bottom edge.

The installed follow-up plugin SHA-256 is
`d5de4ec11250fda2b6a784329a7f86d153a42004838b397ad801add32feda0d4`;
its matching integration fixture is
`5358d7428b7add50f5fc48b72576db85c17055cdea1ec26a4dd03c7c1b09e485`.
The input model passes 135 checks, the rendering boundary passes 6 checks,
and native unified input passes 9 checks in `/tmp/hs-i.8ni_t2ek`.
Three native visual checks in `/tmp/hs-i.wanm9acz` cover absent idle/search
badges, pending-prefix guides, footer labels, and compact guide placement at
480×640 and 480×400.

After the ABI-checked live reload, overview, Space, g, search, and Escape were
checked in the user's session. All 14 client workspace assignments, active
space, per-output selections, monitor desktops, and focus were preserved.
Closing released keyboard/cursor ownership and all capture/texture bytes.
The compositor reports no configuration errors. Screenshots and before/after
status are in
`/home/simon/.local/state/hyprspace/implicit-mode-backup-20261022-222556`.

Preview softness was assessed without changing capture or sampling behavior.
The live capture counters show zero downsampled, failed, fallback, or omitted
captures. Dense desktop text loses detail when reduced to thumbnail size.
The earlier validation below retains its actual build identities.

The overview remains the existing Super+A entry. It opens in Normal mode with
the Spaces drawer closed. The input controller owns the key grammar for
workspaces, windows, spaces, history, help, and appearance editing.

## Behavior

- Normal: hjkl/arrows, Tab/Shift+Tab, gg/G, Enter, and Escape/Ctrl+[.
- Space opens an immediate command guide; s opens Spaces, h History, e the
  current/highlighted space editor, a creates a space, w/v choose workspace or
  window views, g cycles board grouping, r changes board ordering, n prepares
  a workspace, x closes an empty workspace, m transfers, d removes a space,
  and ? opens help. Escape cancels a pending prefix.
- / enters Insert search. Enter or Escape ends text editing and retains the
  query; Normal Enter activates a result. i resumes editing and n/N cycle
  results. Insert text never becomes navigation or leader commands.
- The space editor starts on Name in Normal; i edits the name. Tab reaches
  Icon and Color; hjkl chooses icons and h/l chooses colors. Ctrl+Enter saves
  the complete name/icon/color draft together. New space starts in Name Insert.
- Normal Escape restores the parent panel, including its filter, selection,
  mode and scroll. Help preserves a draft beneath it. Actual space switches
  and successful creation return to the workspace grid in Normal mode.
- Ctrl+O/Ctrl+I replay older/newer real visits. The bounded 100-entry history
  persists in spaces.json. Native workspace and space switches are recorded;
  provisional selection and incidental window focus are not. Explicit preview
  and switcher commits can retain a compositor-qualified stable window target.
- Replay follows stable workspace identities, uses the existing multi-output
  space restoration, falls back from closed windows or missing outputs, skips
  deleted spaces, keeps the overview open, and does not append another visit.

## Structural safeguards

Feature renderers supply logical shape geometry to the shared pass adapter.
Only that adapter constructs native rectangle and border elements. A source
boundary test rejects bypasses; fractional preview texture coordinates stay
separate. Neutral control surfaces are #1b1c1f in dark mode and #f4f5f7 in light
mode, with existing contrast correction and independently colored icons.

The controller ties repeated keys to a context/mode/field epoch while the
compositor hook retains per-device press/release ownership. Modifiers do not
consume prefixes; screenshots, media keys, native modified shortcuts, IME
handoff, and held inspection controls retain their native routing. Parent
snapshots are deduplicated and bounded. A gesture receives Escape cancellation
before any attempt to leave its view.

## Validation

The installed plugin SHA-256 is
`77c84377d6c6c4cb0f36e7cb3780239dd5267ad7edd9a8b0423774295d4292a8`.
The matching integration fixture is
`2743e81060f16b8f63b57a7cd42d976b7154831953915988caae67af3a148372`.
The installed Rust launch helper is
`7a2a8fefce95d9c50ae9b168c454e1b43db96aa6847b43f311ef956ab0641c6b`.

`make test` passes 844,826 C++ checks, 2 Rust checks, 69 build/reload checks,
and all included Python suites. The shared grammar group passes 135 checks;
the focused persistent-history group passes 163. Cargo formatting, Python
compilation and `git diff --check` pass. The mandatory rendering-boundary
source test passes all 6 checks.

| Native validation | Passed | Artifact directory | Plugin hash prefix |
|---|---:|---|---|
| Unified keyboard, drafts, parents, foreground leases, direct History APIs | 9 | `/tmp/hs-i.eyyctn4p` | `77c84377` |
| Lua startup/reload/recovery and scoped navigation/history APIs | 36 | `/tmp/hs-i.tt2gj47l` | `77c84377` |
| Full performance and cleanup | 13 | `/tmp/hs-i.8f_ti4p8` | `77c84377` |
| History, title invalidation, real output hotplug, stale destinations | 9 | `/tmp/hs-i.ohfqotqw` | `052825ff` |
| Fresh compositor restores all 100 visits and exact cursor | 2 | `/tmp/hs-i.cwiddo05` | `052825ff` |
| Tracked caption-corner regression in opposite control themes | 3 | `/tmp/hs-i.19c5db5b` | `052825ff` |
| Shared native border factory, five scale/rotation cases | 6 | `/tmp/hs-i.0w505uuq` | `052825ff` |
| Full window board | 38 | `/tmp/hs-i.2sd61k6c` | `621d3863` |
| Help and native keyboard routing | 14 + 10 | `/tmp/hs-i.p4yc7gny`, `/tmp/hs-i.4hk4j597` | `621d3863` |
| Selection consistency | 7 | `/tmp/hs-i.5h0lwg_8` | `621d3863` |
| Strengthened window lifecycle / held-key foreground handoff | 8 | `/tmp/hs-i.vtsfa979` | `7cdb1395` |
| Independent spaces and state restoration | 14 | `/tmp/hs-i.q6vcthk0` | `7cdb1395` |
| Foreground layers, screenshots, cursor and panels | 8 | `/tmp/hs-i.7p8pqu95` | `7cdb1395` |
| Empty workspace and close workspace contracts | 35 + 14 | `/tmp/hs-i.abqgc1ac`, `/tmp/hs-i.3d_4clow` | `7cdb1395` |
| Panel/space IPC and live occupancy projection | 9 | `/tmp/hs-i.vvq7cm53` | `621d3863` |
| Configuration | 10 | `/tmp/hs-i.6yxj55mv` | `3d053386` |

The final changes after `052825ff` concern direct History on/off handling:
`off` pops an Insert filter, and `on` reveals History above Help while retaining
an existing query. The expanded final keyboard and Lua groups cover those APIs.
Rendering and persistent-history backend code are unchanged from their listed
pixel/restart checks. The earlier broader groups are retained with their actual
build identities rather than represented as reruns of the final binary.

## Native rendering proof

Native controls were reviewed at logical 1600×900, physical 1920×1080, scale
1.2, Sans 12 and a real 56-pixel reserved panel. Dark/light, opposite-theme
overrides, long History, Help, Board, editor fields, and compact 480×640 were
captured. History's Back row remains clear of the mode indicator, and the
obsolete single-key New workspace badge is gone.

The native input field has continuous straight focus edges in both modes:
dark 434/434 horizontal and 30/30 vertical samples; light 435/435 and 31/31.
The shared border factory has 100% coverage on all four straight edges at
scales 1, 1.2, 1.25, 1.5 and rotated 1.25. The intentional raw native A/B control
still clips its far edges, demonstrating that the test reproduces the original
failure rather than accepting an invisible border.

A separate child-fill defect was exposed by forced themes and fixed by drawing
the rounded card fill through the shared adapter, clipped to its caption band.
The tracked `control-surfaces` group checks the real GPU pixels: caption interiors
are RGB 244/245/247 or 27/28/31; pixels outside the bottom curves match the opposite
backdrop, RGB 17/17/25 or 239/241/245. The old square fill fails these checks.

## Performance and resources

Overview/Board measurements run from the input event to compositor RENDER_POST;
space-switch measurements run from dispatcher entry to the first target-generation
RENDER_POST on every awake output. Both exclude GPU completion and physical
presentation. Ordinary overview p95 was 17.93 ms
with 3 windows and 17.74 ms with 12; ordinary Board p95 was 14.76 ms and 15.16 ms.
The 36-window stress cases measured 16.70 ms and 15.38 ms respectively.
Warm space-switch p95 was 25.574 ms against a 33.333 ms two-refresh budget.

Growing from no inactive clients to four inactive spaces containing 24 windows
left the 12-window active capture allocation unchanged at 5,729,188 bytes.
Steady membership rebuilds remained zero. One hundred Board cycles, one hundred
overview cycles, and one hundred space switches passed bounded-resource and
input-release checks. The first performance attempt met all six latency
workloads but stopped at an obsolete single-Enter search fixture; that fixture
now verifies the new Normal transition before the second Enter commits. The
complete final 13-check run passed without changing performance thresholds.

## Live deployment

The ABI-guarded atomic reload succeeded against Hyprland 0.56.2, commit
`efb50993780079460b0cbed1363e2166a2de1d9f`. The launch helper was installed atomically.
Backup and live evidence are in
`/home/simon/.local/state/hyprspace/vim-navigation-backup-20261022-215824`.

Live keyboard checks verified collapsed Normal opening, Space s / Ctrl+Space,
icon editing and cancelled-draft preservation, Help's navigation guard, literal
Insert text, retained results, parent restoration, History, empty-space isolation,
bar occupancy, and Ctrl+O/Ctrl+I replay. The user's original space, monitor-local
workspace selections, focus, and all 14 windows present when validation began
were preserved. No space names/icons/colors were changed. After dismissal,
keyboard/cursor ownership and capture/texture allocations were zero. Native
configuration errors were empty; the installed Rust helper also returned the
new status successfully.
