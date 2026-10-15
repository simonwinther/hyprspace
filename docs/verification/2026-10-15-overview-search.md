# Direct window search release review

The source change is commit `2e805c9f9b403c1d13dde384b41da5b806450cc7`,
reviewed on 2026-10-15 for the 1.8.0 release candidate.

Pressing `/` from the workspace overview now opens All windows with the search
field focused. The same layout-aware shortcut handles Shift and AltGr layouts
in both views. Searches can combine application names and window titles;
Enter opens the selected native window. Escape clears search, returns to the
workspace view, then closes the overview.

The existing window-view integration suite includes the new entry path,
cross-output selection, combined application/title queries, repeated key
presses, input isolation and Escape behavior.

## Completed local checks

- `make test`: 843,206 C++ checks with zero failures, 69 build/reload checks,
  and all five Python fixture suites passed.
- `make -C test asan`: the same 843,206 checks passed with AddressSanitizer
  and UndefinedBehaviorSanitizer.
- `make -j2 integration-fixtures`: plugin and private test fixtures built
  against the supported Hyprland 0.56.2 compositor.
- `make release-check` and `python3 test/test_release.py`: version metadata
  and all 31 release-tooling tests passed before the version update.

[Source CI](https://github.com/simonwinther/hyprspace/actions/runs/38757808860)
also checks GCC, Clang, sanitizers, the pinned Arch build and the Nix package.
Release-branch and exact-tag workflows check the final candidate separately.

## Publication gate

The full background command was attempted with the pinned Walker/Elephant
binaries and Firefox:

```sh
python3 test/integration/run.py --companions build/companions/bin --firefox
```

The private display host stopped before any runtime assertions because this
environment has no `/dev/dri` render device. wlroots reported
`Cannot create GLES2 renderer: no DRM FD available`. This is an unavailable
runtime prerequisite, not a successful integration result. No visible or
physical desktop session was started.

Keep 1.8.0 as a draft until the candidate completes the private compositor
suites, tagged hyprpm lifecycle and Nix loading checks required by the
[release process](../releasing.md#compatibility-and-installation-gate).
The published 1.7.0 release remains the selected stable installation version.

## Branch reconciliation

The local `release/v1.6.0`, `release/v1.7.0` and old
`release-please--branches--master` branches have exactly the published source
trees of 1.6.0, 1.7.0 and 1.4.0 respectively. Their distinct commit IDs come
from squash merges. `experiment/window-views` is an ancestor of `master`.
The closed, abandoned 1.8.0 proposal repeated previously released changes;
its branch was removed before preparing this candidate.
