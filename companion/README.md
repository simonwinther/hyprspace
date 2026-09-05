# Walker and Elephant launch contexts

The patches in this directory add an optional protobuf activation field, `context`
(field 7), to Walker 2.17.0 and Elephant 2.22.0. Archive URLs and SHA-256 checksums
are recorded in [versions.json](versions.json). Upstream files managed by Omarchy
are not edited.

Walker captures an opaque destination in its result-activation handler. Keyboard,
mouse and keep-open actions share that handler. Elephant carries the field to the
desktopapplications, websearch and runner providers. Non-launch actions retain
their existing behavior. Providers may continue exporting the old `Activate`
function; context-aware providers additionally export `ActivateWithContext`.

Walker also limits its layer's input region to the visible panel during the
overview. A background instance-socket probe keeps this independent of keyboard
focus and restores normal routing when the overview closes or the plugin unloads.

## Build

Install Rust/Cargo, Go (at least the version in Elephant's go.mod), pkg-config,
GTK4 development files, gtk4-layer-shell, protobuf and patch using your system's
package manager. The plugin also needs Python 3 for its launch helper.

From the repository:

```sh
make
python3 scripts/build-companions.py
```

The builder verifies archive hashes, applies patches without fuzz, runs the new
Rust and Go tests, and builds into `build/companions/bin`.
`compatibility.json` records source/patch hashes, compiler versions and binary
hashes; the integration suite verifies that record before launching companions.
`--go /path/to/go`, `--jobs 2`, `--output PATH` and `--prepare-only` are available.
Repeat `--provider NAME` to build additional providers with the same daemon ABI.
Subsequent builds also rebuild every provider already in that output directory,
so changing the compiler cannot leave older provider libraries behind.
If sources or patches change, choose a fresh output
directory; the builder refuses to overwrite a different source tree.

Go plugins must be built with the same compiler, dependencies and shared package
sources as their Elephant daemon. The builder supplies the three affected launch
providers. Rebuild any additional providers you use from that same Elephant source
tree; do not mix the supplied daemon with distribution-built provider `.so` files.

## Use the companions

`make install` installs `hyprspace-launch`, its service-launcher shims and the plugin.
When hyprpm manages the plugin, use the `install-assets` Make target to install
the helper and shims separately; hyprpm copies only the plugin library.
For a development build, put the repository's `build` directory on the companion
processes' PATH. Put the companion binary directory before distribution binaries
on PATH too: Walker invokes `elephant listproviders` during initialization.
Set `ELEPHANT_PROVIDER_DIR` to the companion's `providers` directory for **both**
services: Walker's discovery subprocess loads the provider libraries directly. Preserve
your normal Walker and Elephant configuration and command-line arguments.

Use personal systemd user-service overrides for local validation after the nested
suite passes. Preserve each service's existing arguments and set the binary path,
PATH and provider directory together. Keep the distribution binaries and
Omarchy-managed configuration available for rollback: removing those overrides
and restarting the services restores the packaged applications. Complete the
[release gates](../docs/interactive.md#verification) before distributing a release.
The build script does not install applications, restart services or change shared
process environments.

The helper preserves the original shell command and argv. For `uwsm-app --`,
`uwsm app --` and app2unit executable launches, its per-child PATH shims insert
the helper after the service boundary, retaining launcher options. Desktop-entry
IDs such as `application.desktop:Action` and unrecognized launcher option forms
keep native argument parsing. They may lose correlation at the service boundary.
Elephant normally expands desktop entries into executable commands before this
point. Arbitrary service wrappers that discard both environment and activation
tokens retain native placement.

An absent companion, helper or overview returns an empty context and uses normal
launching. A consumed, expired or disconnected destination is never reused for a
later arbitrary window. Existing windows retain native activation behavior.
