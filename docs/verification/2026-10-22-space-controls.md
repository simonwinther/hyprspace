# Space controls and normal-size preview clarity

The installed plugin SHA-256 is
`0bf8d47ecb59313d5630803d398149ee826a5962e83ba37cea0e8331e3707dc7`.
The matching native fixture is
`e79da14d43fb35e29707beada5e757ff03a618f7ce52bff62472c3d664823e37`.

## Result

- The normal overview uses area downsampling with restrained sharpening. Four
  filters were compared at identical card sizes on native screenshots at output
  scales 1, 1.2 and 1.5. The [comparison report](2026-10-22-preview-clarity.md)
  records the visual choice, timings, resource costs and exact tested builds.
- The space editor offers 48 bundled, pinned Lucide icons. Tab reaches Icon;
  `/` or `i` edits its search, Enter/Escape finishes editing, hjkl selects,
  Page Up/Down pages, and Ctrl+Enter saves the complete draft. Searches include
  semantic aliases such as school and gaming. An unsuccessful search restores
  the icon selected before entering that search, including after Help.
- The compact Spaces chooser has space rows, search and two small lines of key
  hints. The New space, History and Manage action rows are removed. Tab cycles
  only List and Search. Existing leader commands provide those actions;
  `Space p` opens the complete manager.
- Shift+J/K moves the selected space down/up in the chooser or manager and saves
  its order. It retains the selected identity and current space, stops at the
  ends, and moves between matching siblings in a filtered list while preserving
  the relative order of hidden entries. Uppercase letters remain literal while
  editing search text.
- `Space m` from the workspace grid defaults to moving the whole workspace,
  including every window. From the window board it defaults to the selected
  window. The guide names the actual transfer scope, and the dialog retains an
  explicit choice. Workspace transfers preserve native/logical workspace
  identities, contents and the active space.

## Validation

`make test` passed 848,506 C++ checks, 2 Rust checks, 69 build/reload checks and
the included Python suites. The rendering-boundary test passes all 6 checks.

| Native check | Passed | Artifact directory | Plugin prefix |
| --- | ---: | --- | --- |
| Complete space controls, search drafts, reordering, multi-monitor window/workspace transfers, compact layouts | 14 | `/tmp/hs-i.ilp6gjuj` | `0bf8d47e` |
| Nine dark/light and compact screenshots; pager glyphs reviewed | 9 images | `/tmp/hs-i.o36bapjz` | `0bf8d47e` |
| Shared input grammar, Help/draft parents, foreground ownership | 9 | `/tmp/hs-i.bgah0r1v` | `87790457` |
| Configuration, filter defaults/runtime values and safe invalid-value fallback | 11 | `/tmp/hs-i.eh601ox2` | `184822d1` |
| Window board rendering, grouping, lifecycle and resources | 39 | `/tmp/hs-i.cl8ok543` | `184822d1` |

The final change after `87790457` only centers the icon pager arrows inside
their full button bounds. The final space-controls group and screenshot review
cover that final binary. The earlier capture/resources/GPU checks and the
normal-size A/B comparison retain their actual hashes in the linked report;
filter code did not change afterward.

Native screenshots are unchanged output captures:

- Icons: [dark grid](../screenshots/space-controls/icons-grid-dark-scale-1.2.png),
  [light grid](../screenshots/space-controls/icons-grid-light-scale-1.2.png),
  [light search](../screenshots/space-controls/icons-search-light-scale-1.2.png).
- Chooser: [dark](../screenshots/space-controls/chooser-dark-scale-1.2.png),
  [light](../screenshots/space-controls/chooser-light-scale-1.2.png).
- Compact: [480×640 grid](../screenshots/space-controls/icons-grid-dark-compact-480x640.png),
  [480×400 search](../screenshots/space-controls/icons-search-dark-compact-480x400.png).

Screenshot and artifact hashes are recorded in
`build/space-controls-native-result.json` and `build/space-controls-demo.json`.
All private compositor sessions were closed.

## Live installation

The ABI-checked atomic reload succeeded. Live checks exercised the sharp
overview, compact chooser, manager, icon search and whole-workspace transfer
picker without submitting changes. All 8 windows, 3 spaces, focus, monitor
desktops, space order and per-output selections were preserved. Closing released
all owned capture/filter/UI bytes and keyboard/cursor ownership. Filter failures
and compositor configuration errors were zero.

Live backup, screenshots and before/after status:
`/home/simon/.local/state/hyprspace/preview-icons-final-backup-20261022-230358`.
