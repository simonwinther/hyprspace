# hyprspace

A live workspace overview and an Alt+Tab switcher for Hyprland.

Supports **Hyprland 0.56.2**, commit
`efb50993780079460b0cbed1363e2166a2de1d9f`, on x86_64 Linux. The plugin must match
your compositor's commit and library ABI. Other Hyprland versions require
separate compatibility testing.

## Install

Use **hyprpm** on Arch and other Linux setups with the supported Hyprland build,
or the **tagged flake** on Nix. [Published releases](https://github.com/simonwinther/hyprspace/releases)
are the stable-version list. The next release candidate is `v1.0.1`; the
commands below become available after that release is reviewed and published.
An unversioned repository install tracks development.

### Arch Linux and hyprpm

On a system running the supported Hyprland version, install the dependencies
and select the release explicitly:

```bash
sudo pacman -S --needed hyprland base-devel git cmake cpio cairo pango gdk-pixbuf2 librsvg libei nlohmann-json jq python
hyprpm update
hyprpm add https://github.com/simonwinther/hyprspace.git v1.0.1
hyprpm enable hyprspace
hyprpm reload
```

Check that your package repositories still provide the supported compositor
before installing. Avoid partial system upgrades. On other distributions,
install the equivalent development packages, then use the same hyprpm commands.

Add this after your other bindings in `~/.config/hypr/hyprland.conf`:

```ini
exec-once = hyprpm reload
unbind = SUPER, A
unbind = ALT, TAB
unbind = ALT SHIFT, TAB
bind = SUPER, A, hyprspace:overview
bind = ALT, TAB, hyprspace:switch
bind = ALT SHIFT, TAB, hyprspace:switch, prev
```

Apply the bindings and check activation:

```bash
hyprctl reload
hyprctl plugin list
hyprctl configerrors
```

The plugin list should include `hyprspace` and configuration errors should be
empty. Startup loading takes effect at your next login. Use hyprpm to manage
loading throughout; remove any local-build `plugin =` line when switching to it.

A selected tag stays selected during `hyprpm update`. To choose a newer published
release, remove and add the repository with that release's tag, then enable and
reload it. A Hyprland update still needs matching headers and a compatible
plugin release. See [updates, removal and troubleshooting](docs/install.md).

### NixOS and Home Manager

Add these inputs to your system flake:

```nix
inputs.hyprland.url = "github:hyprwm/Hyprland/efb50993780079460b0cbed1363e2166a2de1d9f";
inputs.hyprspace.url = "github:simonwinther/hyprspace/v1.0.1";
inputs.hyprspace.inputs.hyprland.follows = "hyprland";
```

In Home Manager, with `inputs` passed through `extraSpecialArgs`:

```nix
{ inputs, ... }:
let
  plugin = inputs.hyprspace.packages.x86_64-linux.hyprspace;
  compositor = plugin.compositor;
in {
  wayland.windowManager.hyprland = {
    enable = true;
    package = compositor;
    plugins = [ plugin ];
    extraConfig = "source = ${plugin}/share/hyprspace/bindings.conf";
  };
}
```

Home Manager loads the plugin at startup and the packaged bindings use its
defaults. Select that same `compositor` for the NixOS session. The
[Nix guide](docs/nix.md) shows the complete wiring and validation status.
Use this repository's flake: Nixpkgs' `hyprlandPlugins.hyprspace` is a different
project.

## Overview

Press **Super + A** to see your workspaces and pick a window. Fullscreen and
maximized workspaces spread their previews apart so every window stays visible.
Hold **Alt + Tab** to cycle windows, then release Alt to focus your selection.

The overview stays open while you launch applications, rearrange windows,
resize and use normal Hyprland bindings. Walker targeting uses the optional
[companion integration](companion/README.md). See the
[verification record](docs/verification/2026-09-06.md) for tested builds and limits.

![Three workspaces with live window previews in hyprspace](docs/screenshots/overview.png)

## Use it

| Control | Action |
|---|---|
| Super + A | Open or close the workspace overview |
| Click a preview | Focus that window |
| Arrows or Tab, then Enter | Select and open a workspace |
| 1 through 9, or 0 | Open workspace 1 through 10 |
| Super + left drag | Rearrange a window or move it across workspaces and outputs |
| Super + right drag | Resize a window in its preview |
| Super + L (optional binding) | Cycle dwindle/scrolling on the indicated workspace |
| Alt + Tab / Alt + Shift + Tab | Cycle windows forward / backward |
| Release Alt | Focus the selected window |
| Escape | Dismiss either overlay without selecting |

The overview opens on all monitors by default, with each showing its own
workspaces. Alt+Tab stays on the current workspace unless you change
`switcher:current_workspace_only`.

In a scrolling workspace, use the wheel or two-finger scrolling to pan its preview.
Edge arrows reveal hidden columns. Page Up/Page Down select columns and Enter
opens the selection; Tab/Shift+Tab still move between workspaces.

![Fullscreen Neovim alongside Files on its workspace](docs/screenshots/fullscreen.png)

Fullscreen and maximized windows keep their desktop state while you browse the
overview. Their badges show which windows are using those modes.

![The Alt+Tab switcher with app icons and the selected window title](docs/screenshots/switcher.png)

The screenshots are real captures from an isolated Hyprland session using demo
content. See [capture details](docs/screenshots/README.md).

## Configuration and development

- [Controls, configuration and known limitations](docs/guide.md)
- [Interactive session, launch contexts and verification](docs/interactive.md)
- [Example Hyprland configuration](contrib/hyprspace.conf)
- [Build checks and contributing](CONTRIBUTING.md)
- [Release process](docs/releasing.md) and [changelog](CHANGELOG.md)

Source releases are the supported distribution format. A prebuilt plugin from
another system can have an incompatible ABI even when its Hyprland version
number matches yours.

## License

[MIT](LICENSE). [NOTICE.md](NOTICE.md) records the ideas taken from hyprview and
hyprshell and preserves their attribution.
