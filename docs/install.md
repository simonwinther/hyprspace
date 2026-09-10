# Managing a hyprpm installation

Use a tag listed in [published releases](https://github.com/simonwinther/hyprspace/releases).
The `v1.0.2` examples require that release to be published first.
Hyprpm builds locally against your installed compositor and development libraries.
The supported version is Hyprland 0.56.2, commit
`efb50993780079460b0cbed1363e2166a2de1d9f`, on x86_64 Linux.

## Updates and version selection

`hyprpm update` retains the revision passed to `hyprpm add`. It does not choose
a newer stable release for you. Explicit revisions also take precedence over
repository commit pins in the [supported hyprpm implementation](https://github.com/hyprwm/Hyprland/blob/efb50993780079460b0cbed1363e2166a2de1d9f/hyprpm/src/core/PluginManager.cpp).

To change versions, disable and remove the installed repository, then repeat
installation with the desired published tag. This example selects `v1.0.2`:

```bash
hyprpm disable hyprspace
hyprpm reload
hyprpm remove hyprspace
hyprpm add https://github.com/simonwinther/hyprspace.git v1.0.2
hyprpm enable hyprspace
hyprpm reload
hyprctl plugin list
```

Keep the configuration snippet in place when switching versions. Check the
selected revision with `hyprpm list`. Log out and back in, then check
`hyprctl plugin list` again to verify startup loading.

After a supported compositor or library rebuild, log into that compositor,
run `hyprpm update` and `hyprpm reload`, and check activation again. Updating
the compositor to an unsupported commit requires a compatible plugin release;
rebuilding alone does not extend compatibility.

## Removal

```bash
hyprpm disable hyprspace
hyprpm reload
hyprpm remove hyprspace
```

Remove the hyprspace bindings and unbinds you added to `hyprland.conf`. Remove
`exec-once = hyprpm reload` only if you no longer use other hyprpm plugins. Run
`hyprctl reload` and check that `hyprctl plugin list` no longer lists hyprspace.

## Troubleshooting and optional configuration

An ABI mismatch means the compositor, headers or libraries differ. Verify
`hyprctl version`, the installed package versions and the release's supported
commit. Do not bypass the runtime checks. See the
[upstream plugin guide](https://wiki.hypr.land/Plugins/Using-Plugins/) for hyprpm
header setup on other distributions.

The minimal installation uses existing defaults. For layout cycling and
appearance settings, see the [configuration reference](guide.md) and
[full example](../contrib/hyprspace.conf). Manual builds are in
[CONTRIBUTING.md](../CONTRIBUTING.md#manual-installation).

Hyprpm copies the library only. Launch contexts need the helper assets installed
from the matching revision using `make install-assets`; the plugin finds them
in `~/.local/share/hyprspace`. Follow the [companion guide](../companion/README.md)
for Walker and Elephant integration.
