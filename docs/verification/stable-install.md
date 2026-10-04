# Stable installation verification

This record covers `v1.3.0`, commit
`852544c3c7ca37c1b05702e4b4abf0669adefa66`, checked on 2026-10-04.
The Git tag is publicly fetchable. Its GitHub release remains a draft until the
[physical-session gate](../releasing.md#compatibility-and-installation-gate) passes.

The tagged source tree is `37d6c9a02955e720b69a2b04890dbf06acfc31ef`, identical
to the clean release PR candidate `4fb4c67470277ec57bc2826f8414cd46934efd88`.
Every inventoried artifact retained its checksum after the release merge. The
local plugin's SHA-256 is
`c816c7aafb7376736cca2c42afdb9757db4bcb858f970e35a0f5b0c111bd3e29`.

## Private compatibility checks

The final binary completed 270 required checks across the full background run
and separate dispatcher, activation and Discord runs. Coverage includes all 27
layout/output pairs, wheel zoom and panning, lifecycle and input ownership,
foreground, resize, lock, audit, pinned companions and application reuse.

Walker 2.17.0, Elephant 2.22.0 and their provider binaries matched the companion
compatibility record. Firefox 154.0 and the official Discord 1.0.160 client used
temporary profiles without signing in.

The original full command timed out at an outdated Discord updater before its
activation group ran. Activation passed separately. The current Discord fixture
then encountered a DNS outage; the unchanged retry passed after connectivity
recovered. No assertion or timeout changed. The saved results preserve both
failures and the successful completions; this is coverage across runs, rather
than a successful exit from the original full command.

| Run | Passed checks | Generation |
|---|---:|---|
| Full run before Discord startup stopped | 255 | `86053630d8fb494da4398c32e90ace30` |
| Isolated dispatchers | 7 | `86053630d8fb494da4398c32e90ace30` |
| Activation completion | 6 | `751b633c15ef44508abde1a885320929` |
| Current Discord reuse completion | 2 | `a08c45952185409ea71e424d1a9f92ad` |

The release PR and dispatched checks passed GCC/Clang host tests, ASan/UBSan,
the pinned Arch build, and Nix package/ABI checks. Host sanitizers do not
instrument the compositor plugin.

## hyprpm

The real public repository URL and v1.3.0 tag passed in a disposable Arch
container with Hyprland 0.56.2, commit
`efb50993780079460b0cbed1363e2166a2de1d9f`. Dependencies used the pinned
2026-09-01 Arch snapshot; the documented `nlohmann-json 3.12.0-2` prerequisite
was added only to that container.

Fresh header installation passed from an empty manager store. The complete
lifecycle then passed 11 checks with those cached headers:

- Tagged repository addition, enable and reload, with exact source commit and ABI.
- Minimal Super+A and both Alt+Tab directions, plus plugin unload/reload.
- Update retaining v1.3.0 and its exact source commit.
- Loading through `exec-once = hyprpm reload` after a private compositor restart.
- Disable, reload and removal, with no remaining plugin mappings or repository.
- Explicit selection of public Git tag v1.2.1, its different commit and binary,
  followed by removal.

The actual manager-built 1.3.0 library's SHA-256 was
`eb65713bb697e6e36411768b5fef47bb4531ef8802884387a0831b36c99bbd86`
at activation, update and startup. Manager state identified the real URL,
revision v1.3.0 and release commit; loaded plugin metadata and process mappings
identified this managed file separately from the test bootstrap.

The earlier tag resolved to `bc7a73bb6d2a9aaa66b058ba38f2b01046a9fdb3`,
loaded version 1.2.1 and SHA-256
`103b0823d697c87dd84726f16f01a76c28728008707a7a54f296c59d153ab1c8`.
These checks used publicly fetchable Git tags. The GitHub releases were drafts
during testing.

Bootstrap generation: `f8525781bed940bfbb12a7cc41941074`. The first lifecycle
attempt passed fresh headers and startup, then its temporary script put the
replacement virtual pointer in a client cleanup slot. The corrected rerun
retained the pointer across restart and completed the lifecycle. Failed and
successful results are both preserved; no plugin source changed.

## Nix

The exact tagged source built with Nix 2.24.14 in a disposable container,
using the locked inputs and a copy of the historical store cache. Host Nix
configuration and the historical validation containers were unchanged.

Package:
`/nix/store/alamk5k4bhdf8ppz6bbl9lpw04lqyc2s-hyprspace-1.3.0`.
Matching compositor:
`/nix/store/sirqxk22z7x7yh1nz4pimw0akw4apgqw-hyprland-0.56.2+date=2026-08-05_efb5099`.
Installed library SHA-256:
`4dc2880e4ef6b49750c0d45de22c5e96a29bf196cc68c91f5f528d41fb75cdf7`.

Completed checks:

- Exported package and default alias agree.
- Library, helper, launcher shims and minimal bindings are installed correctly.
- The helper uses its Nix Python interpreter and runs without a desktop.
- Plugin and compositor compiler derivations and ABI metadata agree.
- The hooked pointer function has the required branch-protection instruction.
- An unsupported compositor input fails evaluation with the documented error.
- The package loads into its matching Nix compositor with clean configuration;
  minimal Super+A, both Alt+Tab directions and unload/reload pass.

The matching package/compositor closure was copied into a separate disposable
runtime container, with hashes checked against the package manifest. The first
transfer preflight rejected a legitimate systemd symlink to `/etc/environment`.
The corrected transfer retained that single inert symlink, refused archive
members beneath it and kept writes within `/nix/store`. The original closure
archive and package checksums stayed unchanged; no `/etc` file changed.

The private install run passed four checks, generation
`6af593810ff24dfbbb6044e9e23df59b`, with the packaged library's exact checksum
recorded in `generation.json`. Rendering and input stayed inside the private
server. No host compositor or desktop sockets were used.

## Release artifacts and remaining observations

[The exact-tag release workflow](https://github.com/simonwinther/hyprspace/actions/runs/37220635660)
passed all checks before attaching source and checksums. The downloaded
`hyprspace-1.3.0.tar.gz` is 15,130,884 bytes, SHA-256
`25d2791488698f4409cdcd7921622a4a74369d2aa6ac513fc73c3312b82b4a5b`.
It matches `SHA256SUMS`, identifies the exact release commit in its archive
metadata, and contains all 144 tracked files with their exact Git blob contents
and executable flags. No local objects or compiled libraries are included.

The draft also contains the 1080p zoom/pan video, private compatibility results,
companion compatibility record, tagged installation results and tag/archive
identity records. Supplemental assets have their own SHA-256 checksum file.
Nested `generation.json` and `results.json` files preserve the measured artifacts
and outcomes; original failed setup attempts remain included.

No live plugin reload, visible runner or physical desktop test was performed
for this release preparation. The three-monitor cursor/layout observations,
installed launcher, notifications and screenshot selector, and live application
observations remain required before publication. A NixOS or Home Manager system
activation was not tested.

[The earlier installation record](https://github.com/simonwinther/hyprspace/blob/v1.3.0/docs/verification/stable-install.md)
covers its recorded 1.0.0 snapshot and is separate from this candidate's evidence.
