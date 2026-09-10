# Nix installation

This repository exports `packages.x86_64-linux.hyprspace` and `default`.
The Nixpkgs package with the same name belongs to another project. Always use
`github:simonwinther/hyprspace` for this plugin.

The locked compositor is Hyprland 0.56.2, commit
`efb50993780079460b0cbed1363e2166a2de1d9f`. The flake uses its Nixpkgs and
`mkHyprlandPlugin` with the builder's compositor overridden, so compiler,
headers and libraries come from the same package set. Runtime ABI checks remain
enabled. The pinned upstream derivation supplies Glaze 8 although Hyprland
requires Glaze 7. The package overrides that dependency to 7.2.0 and exposes
the matching compositor as `hyprspace.compositor`; use it for both session
and Home Manager. It also enables the same x86 branch-protection compiler flag
as Arch. Without that flag, Hyprland's hook trampoline cannot relocate an early
call in the pointer-coordinate function. Only x86_64 Linux is supported because
the plugin uses compositor hooks.

## Select the session and plugin together

In your system flake, add:

```nix
inputs.hyprland.url = "github:hyprwm/Hyprland/efb50993780079460b0cbed1363e2166a2de1d9f";
inputs.hyprspace.url = "github:simonwinther/hyprspace/v1.0.2";
inputs.hyprspace.inputs.hyprland.follows = "hyprland";
```

Use this tag after its release is published. The `follows` relationship lets
consumers own the compositor input; evaluation rejects any
revision other than the supported commit. Keep its dependency lock entries
consistent too. Do not independently override the session's libraries.

Pass `inputs` through `specialArgs` in `nixosSystem` and through
`home-manager.extraSpecialArgs` (or `extraSpecialArgs` in a standalone
`homeManagerConfiguration`). In your NixOS module:

```nix
{ inputs, ... }:
{
  programs.hyprland = {
    enable = true;
    package = inputs.hyprspace.packages.x86_64-linux.hyprspace.compositor;
    portalPackage = inputs.hyprland.packages.x86_64-linux.xdg-desktop-portal-hyprland;
  };
  home-manager.extraSpecialArgs = { inherit inputs; };
}
```

Use the Home Manager snippet in the [README](../README.md#nixos-and-home-manager)
with that same compositor. Place its `extraConfig` source after other binding
sources. Home Manager manages startup plugin loading; no hyprpm or custom module
is needed. This follows the [upstream Nix plugin model](https://wiki.hypr.land/Nix/Plugins/).
Rebuild your system and Home Manager configuration, then log into the selected
session. Check `hyprctl plugin list` and `hyprctl configerrors`.

To update, select a newer published hyprspace tag and update that input's lock.
To remove it, remove the plugin entry and bindings source and rebuild your
configuration; restart the session to unload the old library.

## Package contents and verification

The output contains:

- `lib/libhyprspace.so`
- `lib/hyprspace-launch`, with its Nix Python interpreter
- `lib/launch-bin/{uwsm-app,uwsm,app2unit}` shims
- `bin/hyprspace-launch`, pointing to the helper
- `share/hyprspace/bindings.conf`

Building writes only into the Nix output and needs no running desktop. Companion
applications are not packaged. For a local source checkout:

```bash
nix build .#hyprspace
python3 scripts/check-nix.py
```

Nix includes only tracked files when building a Git checkout. New flake files
must be present in the source used for validation. The CI Nix job builds and
checks the installed files, compiler agreement, ABI metadata, hook entry and
rejection of an unsupported input. Successful package and private compositor validation are
required before publication; see the [installation verification record](verification/stable-install.md).
