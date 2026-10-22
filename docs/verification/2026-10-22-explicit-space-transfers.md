# Explicit window and workspace transfers

The built and installed plugin SHA-256 is
`f07ffb5e43a15e75d0b09c674db8826591e03c04b56452c7254181e44830f0db`.
The matching native fixture is
`6742020dea22658e0daefb4444711f63171d2738a7542dc4f910774221b495a5`.

`Space m` moves the selected window to another space. `Space Shift+M` moves
the selected workspace and all its windows. Both commands work in the workspace
grid and window view, and the destination picker retains an explicit scope
switch. `Space w` still returns to the workspace grid.

Opening the picker captures the overview selection before changing overlays.
Confirmation requires the original window to remain mapped, visible and in the
original workspace and source space. Missing or stale targets produce an error;
the command cannot substitute desktop focus, another window or a workspace.
Workspace transfers preserve logical/native identity and contents.

Validation on this exact build:

- `make test`: 848,605 C++ checks, 2 Rust checks, 69 build/reload checks and all
  included Python suites passed. The rendering-boundary suite passed 6 checks.
- Native transfers: 11 checks, including both scopes in both views, physical
  Shift input, a selected window different from desktop focus, three outputs,
  empty selections, closed/relocated targets and cancellation retaining search,
  grouping, ordering, selection and nonzero scroll. `/tmp/hs-i.15bbg8xy`.
- Native space management: 14 checks. `/tmp/hs-i.5rudvin9`.
- Native shared input: 9 checks. `/tmp/hs-i.zk581f34`.
- Visual review: 7 checks and 18 screenshots across dark/light themes at
  1920×1080 scale 1.2, 480×640 scale 1 and 480×400 scale 1.
  `/tmp/hs-i.kb7_lcpr`. At 480×400 the guide shortens long labels, retaining
  the differing window/workspace scope. Picker labels fit in full.

Representative unchanged native screenshots:

- [Command guide](../screenshots/space-transfers/transfer-1920x1080-s1.2-dark-guide.png).
- [Selected window](../screenshots/space-transfers/transfer-1920x1080-s1.2-dark-window.png).
- [Whole workspace, light theme](../screenshots/space-transfers/transfer-1920x1080-s1.2-light-workspace.png).
- [Compact guide](../screenshots/space-transfers/transfer-480x400-s1-dark-guide.png).
- [Compact picker, light theme](../screenshots/space-transfers/transfer-480x400-s1-light-window.png).

An ABI-checked atomic reload installed the tested binary. Live checks opened and
canceled both commands in both views without submitting transfers. A fresh
before/after smoke snapshot preserved all 10 windows, 3 spaces, workspace
assignments, monitor slots and focus. Closing released all owned capture,
filter and UI bytes and keyboard/cursor ownership. Configuration errors were
empty. Private compositor sessions were closed.

Logs/results: `build/split-transfer-*`, `build/transfer-visual-review.log`.
Live backup and smoke evidence:
`/home/simon/.local/state/hyprspace/transfer-shortcuts-backup-20261022-232640`.

History behavior and desktop shortcuts were not changed in this implementation.
