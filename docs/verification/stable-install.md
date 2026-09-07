# Stable installation verification

This records the installation changes prepared on 2026-09-07, based on commit
`11febee` with uncommitted changes. It does not declare a published release or
replace the [interactive compatibility gate](../releasing.md#compatibility-and-installation-gate).

## hyprpm candidate

The documented Arch dependency command and hyprpm lifecycle passed in an
isolated Arch container with Hyprland 0.56.2, commit
`efb50993780079460b0cbed1363e2166a2de1d9f`, and library ABI
`aq_0.14_hu_0.14_hg_0.5_hc_0.1_hlg_0.6`.

The test used disposable local Git tags `v1.0.0` and `v1.0.1` at candidate
snapshot `14d3faf026d56fe58726739261876b06edd28ca6`. Both point to the same
source; the second tag verifies explicit version selection. No tag was created
in the project repository. The managed plugin's SHA-256 was
`13c33c6a7e4068c6e6a5155e7ee49d08c1ac98313be284d2c35ff408a5cbe1f9`.

Completed checks:

- Fresh header installation, tagged repository addition, enable and reload.
- Plugin loading with matching ABI and no configuration errors.
- Super+A and forward/backward Alt+Tab using the minimal bindings and defaults.
- Unload and reload of the installed library.
- `hyprpm update` retaining the selected tag.
- Startup loading through `exec-once` after restarting the private compositor.
- Removal unloading the plugin, followed by explicit selection of the other tag.

The repository was private with no published releases during testing. Fetching
the documented public URL and a published release remains a publication check.
The local candidate test covers the installation mechanics.

## Nix package

The locked package built successfully with Nix 2.24.14 in an x86_64 Linux
container. The host Nix installation could not access `/nix/store`, so the
container provided a working Nix store. The build required no desktop.

The supported upstream input needed two packaging corrections: Glaze 7.2.0 to
satisfy its CMake version requirement, and `-fcf-protection=full` so Hyprland's
pointer-hook trampoline does not copy an unrelocated relative call. The initial
Nix runtime test exposed the latter crash. Both fixes are in the flake; the
compositor source revision and plugin runtime ABI checks are unchanged.

The final output is
`/nix/store/dw2yiv8n04csxsm06sdis7cgvvh7wjkm-hyprspace-1.0.0`.
Its matching compositor is
`/nix/store/sirqxk22z7x7yh1nz4pimw0akw4apgqw-hyprland-0.56.2+date=2026-08-05_efb5099`.
The installed library's SHA-256 is
`1b45cf3c16c3b4a888d0de1b2dc7fb21e6263603c5f2792b013e58cd8057ddfe`.

Completed checks:

- Exported package and default alias agree.
- Library, helper, launcher shims and minimal bindings are installed correctly.
- The helper uses its Nix Python interpreter and runs without a desktop session.
- Plugin and compositor compiler derivations and ABI metadata agree.
- The hooked pointer function has the required branch-protection instruction.
- An unsupported Hyprland revision fails evaluation with the documented error.
- The final package loads into its matching Nix compositor without configuration
  errors; Super+A, both Alt+Tab directions and unloading/reloading pass.

The runtime closure was copied into a separate container with NVIDIA userspace
available for the background display server. Input and test windows remained
inside that private server. A complete NixOS or Home Manager system activation
was not run; their configuration follows the standard upstream plugin interface.

## Release and regression checks

| Check | Result |
|---|---|
| Host test suite and local plugin build | Passed |
| Host ASan/UBSan | 329,103 assertions passed |
| Release metadata, archives and draft retry tests | 25 passed |
| Background runner isolation and packaged-path tests | 12 passed |
| Local plugin with the private installation suite | Passed |
| Release Please 17.3.0 initial, patch and minor version selection | Passed |
| Release Please version-file and declared extra-file updaters | Passed |
| Workflow actionlint and whitespace checks | Passed |

Release tests cover initial and subsequent metadata, changelog headings,
reproducible archives of the exact tagged commit, dirty or mismatched checkouts,
failed packaging, missing drafts, partial uploads, changed tags and conflicting
or published assets. The workflow dependency check verifies that artifact jobs
require successful checks. GitHub token dispatch and permissions were reviewed
and linted; no remote workflow or release was created during this work.

Local diagnostic logs and test scripts are retained under
`build/verification/stable-install/`. That directory is ignored by Git; attach
the relevant results to the release review. Routine rendering and input tests
used a private display server. No physical desktop or companion application
suite was run for these installation changes.
