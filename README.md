# hyprspace

A Hyprland plugin with exactly two things in it:

* **Super** → a full-screen overview showing the live contents of every window on
  every workspace, grouped and labelled per workspace.
* **Alt+Tab** → a GNOME-style switcher: app icons, the window title, forward and
  backward cycling, commit on Alt release, cancel on Escape.

No launcher. No search bar. No hidden input field. Nothing types anywhere.

Built and tested against **Hyprland 0.55.2** on Arch Linux with Omarchy.

---

## Why a plugin

Showing the *real* contents of windows — including windows on workspaces that
are not currently visible — requires access to window textures, which only
exists inside the compositor. An out-of-process client (the approach
[hyprshell](https://github.com/H3rmt/hyprshell) takes) can show icons and
titles, but not live window content.

Putting the switcher in the same plugin also makes "commit on Alt release"
reliable: Hyprland's key event bus fires *before* keybind processing and before
the event reaches any client, and it is cancellable, so hyprspace can hold a
real keyboard grab without a layer-shell surface and without racing anything.

The cost is that Hyprland's plugin ABI is tied to the exact compositor commit,
so the plugin must be rebuilt whenever Hyprland is updated. See
[Known limitations](#known-limitations).

---

## Installation

### Requirements

Arch package names; all but `hyprland` are usually already installed on Omarchy.

```
hyprland          # provides the plugin headers this builds against
base-devel        # gcc, make
cairo pango       # text rendering
gdk-pixbuf2       # raster icon loading (png, xpm, ...)
librsvg           # svg icon loading
```

```bash
sudo pacman -S --needed hyprland base-devel cairo pango gdk-pixbuf2 librsvg
```

### Build and install

```bash
git clone <this repo> ~/dev/hyprspace
cd ~/dev/hyprspace
make
make install
```

`make install` runs `make check` first, which refuses to install a plugin built
against a different Hyprland than the one running — a mismatched plugin takes
the whole session down, so this is checked rather than hoped for.

The default install location is `~/.local/share/hyprspace/hyprspace.so`. Override
with `PREFIX=/some/where make install`.

### Enable it

Add to `~/.config/hypr/hyprland.conf`:

```ini
source = ~/dev/hyprspace/contrib/hyprspace.conf
```

That file sets the plugin path, both keybindings and every configuration option
at its default value. Or, minimally:

```ini
plugin = ~/.local/share/hyprspace/hyprspace.so

bindr = SUPER, SUPER_L, hyprspace:overview
bind  = ALT, TAB, hyprspace:switch
bind  = ALT SHIFT, TAB, hyprspace:switch, prev
```

Then `hyprctl reload`, or to load without restarting:

```bash
hyprctl plugin load ~/.local/share/hyprspace/hyprspace.so
```

### With hyprpm

```bash
hyprpm add <this repo url>
hyprpm enable hyprspace
```

`hyprpm` builds against your installed Hyprland source and rebuilds on update
(`hyprpm update`). Note that `hyprpm` needs a working `sudo` on first use, since
it creates its state store under `/root`.

---

## Keybindings

### Hyprland

```ini
# Tap Super on its own -> overview.
# `bindr` fires on release, so Super held as a modifier for another shortcut
# does not open the overview.
bindr = SUPER, SUPER_L, hyprspace:overview

# Alt+Tab -> switcher.
bind = ALT, TAB, hyprspace:switch
bind = ALT SHIFT, TAB, hyprspace:switch, prev
```

### Omarchy

Omarchy already frees Alt+Tab in its default `bindings.conf`:

```ini
unbind = ALT, TAB
unbind = ALT SHIFT, TAB
```

If yours does not, add those two lines before the `bind` lines. Put the
hyprspace bindings in `~/.config/hypr/bindings.conf` (Omarchy's per-user
overrides file), not in the files under `~/.local/share/omarchy/`, which are
replaced on update.

Omarchy binds `SUPER` combinations heavily but leaves a bare Super tap free, so
`bindr = SUPER, SUPER_L` does not collide with anything.

### Dispatchers

| Dispatcher | Argument | Effect |
|---|---|---|
| `hyprspace:overview` | *(none)* | Toggle the overview |
| `hyprspace:overview` | `on` | Open, never toggle closed |
| `hyprspace:overview` | `off` | Close if open |
| `hyprspace:switch` | *(none)* / `next` | Open the switcher, or step forward |
| `hyprspace:switch` | `prev` / `backward` | Open the switcher, or step backward |
| `hyprspace:close` | *(none)* | Dismiss whichever overlay is up, without selecting |

`hyprspace:close` works over the hyprctl socket even while an overlay holds the
keyboard grab, which makes it a reliable escape hatch.

### Controls

**Overview**

| Key | Action |
|---|---|
| `Esc` | Close, keep the current focus |
| `Enter` / `Space` | Focus the selected window and close |
| `Tab` / `Shift+Tab` | Next / previous window, across all workspaces |
| `←` `↓` `↑` `→` | Move to the nearest tile in that direction |
| `Ctrl+h/j/k/l`, `h/j/k/l` | Same, vim style |
| `Home` / `End` | First / last tile |
| Mouse move | Hover highlights, and selects when `follow_mouse` is on |
| Left click | Select and close; clicking empty space dismisses |
| Right click | Close without selecting |

**Switcher**

| Key | Action |
|---|---|
| Hold `Alt`, tap `Tab` | Step forward through most-recently-used order |
| `Alt+Shift+Tab` | Step backward |
| Release `Alt` | Commit |
| `Esc` | Cancel, keep the current focus |
| `Enter` | Commit without waiting for the Alt release |
| `←` / `→` | Step backward / forward |
| Mouse move, click | Hover to select, click to commit |

Every other key is swallowed while an overlay is up. There is deliberately
nowhere to type.

---

## Configuration

All options live under `plugin:hyprspace:` and can be set in a nested block or
with flat keys. Colours take Hyprland's usual `rgba()`/`rgb()` syntax; sizes are
in logical pixels and are scaled for HiDPI outputs automatically.

```ini
plugin {
    hyprspace {
        follow_mouse = true
        warp_cursor  = true

        overview {
            bg_dim           = 0.80
            padding          = 40
            all_workspaces   = true
        }

        switcher {
            icon_size = 96
        }
    }
}
```

### Shared

| Option | Type | Default | Meaning |
|---|---|---|---|
| `follow_mouse` | bool | `true` | Hovering a tile selects it |
| `warp_cursor` | bool | `true` | Warp the pointer onto the chosen window when committing |

`warp_cursor` matters more than it looks. With Hyprland's `input:follow_mouse`
enabled (the default, and Omarchy's), focusing a window while the pointer sits
over a different one means the pointer wins the moment the overlay closes, and
your selection silently does nothing. Warping is how Hyprland's own
`cyclenext` dispatcher solves this. Turn it off only if you also run
`input:follow_mouse = 0`.

### Overview

| Option | Type | Default | Meaning |
|---|---|---|---|
| `overview:bg_dim` | float `0..1` | `0.80` | How strongly the desktop behind is dimmed |
| `overview:bg_color` | color | `rgba(11111bff)` | Colour mixed over the desktop |
| `overview:padding` | int | `40` | Outer padding |
| `overview:gap` | int | `24` | Gap between tiles |
| `overview:band_gap` | int | `28` | Gap between workspace rows |
| `overview:rounding` | int | `12` | Tile corner radius |
| `overview:border_size` | int | `3` | Selection border thickness |
| `overview:active_border` | color | `rgba(89b4faff)` | Selected tile border |
| `overview:hover_border` | color | `rgba(89b4fa80)` | Hovered tile border |
| `overview:workspace_labels` | bool | `true` | Show the per-workspace label column |
| `overview:label_gutter` | int | `56` | Width of that column |
| `overview:label_color` | color | `rgba(cdd6f4ff)` | Label colour |
| `overview:window_titles` | bool | `true` | Title overlay on the selected tile |
| `overview:title_color` | color | `rgba(cdd6f4ff)` | Title colour |
| `overview:title_bg_color` | color | `rgba(1e1e2ee6)` | Title backdrop |
| `overview:include_special` | bool | `true` | Include special/scratchpad workspaces |
| `overview:all_workspaces` | bool | `true` | `false` restricts to the active workspace |
| `overview:font` | string | `Sans 12` | Pango font description |

### Switcher

| Option | Type | Default | Meaning |
|---|---|---|---|
| `switcher:icon_size` | int | `96` | App icon size |
| `switcher:padding` | int | `24` | Panel inner padding |
| `switcher:gap` | int | `12` | Gap between entries |
| `switcher:rounding` | int | `20` | Panel corner radius |
| `switcher:bg_color` | color | `rgba(1e1e2ef0)` | Panel background |
| `switcher:highlight_color` | color | `rgba(89b4fa40)` | Selection highlight |
| `switcher:text_color` | color | `rgba(cdd6f4ff)` | Title colour |
| `switcher:show_title` | bool | `true` | Show the selected window's title |
| `switcher:current_workspace_only` | bool | `false` | Restrict to the active workspace |
| `switcher:font` | string | `Sans 13` | Pango font description |

### Animations

The transitions reuse Hyprland's own animation curves rather than inventing
their own timing, so they match the rest of your desktop:

* overview open/close → `windowsMove`
* switcher fade → `fadeIn`

Retune those in your `animations` block to change the feel.

---

## How it works

```
              ┌── Event::bus() input.keyboard.key (cancellable, pre-keybind)
              │   input.mouse.move / .button
   Hyprland ──┤
              │   render.pre        → capture live window textures
              └── render.stage      → LAST_MOMENT: add the overlay pass element
```

* **Live tiles.** On every frame, each visible window is rendered into its own
  offscreen framebuffer with Hyprland's own window renderer in *standalone*
  mode: full alpha, no decorations, and — crucially — it renders windows that
  sit on hidden workspaces too. Those clients are also un-suspended while the
  overview is up, so they keep producing frames instead of showing a stale one.
* **Hiding the real windows.** Rather than painting over the desktop, the
  overview warps every collected window to zero alpha, which makes Hyprland's
  normal pass skip them. What remains underneath is the wallpaper and your bar,
  which then get dimmed — that is what makes it read like the GNOME overview
  instead of a panel floating over a screenshot.
* **Drawing.** The overlay is a custom `IPassElement` that returns ordinary
  texture and rect pass elements. No raw GL calls, so it stays correct if
  Hyprland gains another renderer backend.
* **Layout.** Windows are grouped into one row per workspace. Row heights are
  proportional to the square root of the window count: sizing a row by the height
  it would need to fill the width gives the busiest workspace the thinnest strip,
  and sizing it by window count starves the single-window rows. Within a row,
  every row count is tried and the one yielding the largest tiles wins; aspect
  ratios are preserved exactly.
* **Icons.** Window class → `.desktop` entry → icon name → icon theme file, with
  Chromium/Edge web-app classes (`chrome-chatgpt.com__-Default`) decoded to their
  underlying site, which is how Omarchy's web apps report themselves. SVG goes
  through librsvg, everything else through gdk-pixbuf. Unresolvable apps get a
  tinted rounded square with their initial rather than a blank slot.

One deliberate implementation note: hyprspace needs `IHyprRenderer::renderWindow`,
which is `protected`. It reaches it with the standard-blessed explicit-instantiation
idiom ([temp.spec]/6) rather than the widespread `#define private public` hack —
which is undefined behaviour and, as of GCC 16's libstdc++, no longer even
compiles. See `src/Access.hpp`.

---

## Tests and build checks

```bash
make            # build the plugin
make check      # verify the built .so matches the running Hyprland's ABI
make test       # host-side unit tests (179 checks)
make -C test asan   # same suite under AddressSanitizer + UBSan
make clean
```

`make test` builds the Hyprland-independent parts — the layout and navigation
maths, `.desktop` parsing, window-class resolution and the cairo/pango
rasteriser — and runs them on the host, no compositor required. It covers aspect
ratio preservation, tiles staying inside the screen, tiles never overlapping,
workspace grouping and ordering, directional navigation including edges and
out-of-range input, pointer hit testing, localised/malformed `.desktop` files,
Chromium web-app class decoding, icon size and format preference, text
ellipsising and SVG/PNG icon loading.

`make check` parses `GIT_COMMIT_HASH` out of the installed Hyprland headers,
compares it to what `hyprctl version` reports, and confirms every renderer symbol
the plugin imports is actually exported by the `Hyprland` binary. The plugin
also re-checks the full ABI string at load time and refuses to initialise on a
mismatch, with a notification telling you to rebuild.

What is *not* covered by automated tests: anything requiring a live compositor —
rendering, input grabs, focus commits. Those were verified by hand against a
running Hyprland 0.55.2 session (overview open/close/toggle, keyboard navigation,
Enter and Escape in both overlays, workspace switching on commit, focus surviving
`follow_mouse`, and a rapid-toggle stress run confirming no window is left
hidden and the compositor stays up).

---

## Cleanup and uninstall

```bash
cd ~/dev/hyprspace

# Unload from the running session (no restart needed)
hyprctl plugin unload ~/.local/share/hyprspace/hyprspace.so

# Remove the installed plugin
make uninstall

# Remove build artefacts
make clean
```

Then delete the configuration you added:

* the `source = .../hyprspace.conf` line, or the `plugin = ...`, `bind` and
  `bindr` lines, from `~/.config/hypr/hyprland.conf` / `bindings.conf`
* any `plugin { hyprspace { ... } }` block

and `hyprctl reload`.

If you installed with hyprpm instead:

```bash
hyprpm disable hyprspace
hyprpm remove hyprspace
```

Finally, remove the checkout: `rm -rf ~/dev/hyprspace`.

hyprspace writes no state, no cache and no files outside its install path. It
does not change any Hyprland setting; the window alpha it uses to hide windows
during the overview is restored when the overview closes, and on plugin unload.

---

## Known limitations

* **The plugin ABI is tied to the exact Hyprland commit.** Every Hyprland
  update requires `make && make install` (or `hyprpm update`). The plugin
  refuses to load on a mismatch rather than crashing your session, so the
  failure mode is "Super does nothing", not a lost session. Built against
  **0.55.2**; earlier and later releases will need source changes, since the
  render, config and event APIs all moved in the 0.55 cycle. For the same
  reason, hyprview and hyprshell's Hyprland-facing code could not be reused
  directly — see `LICENSE` for what was taken from each.
* **Rotated monitors are untested.** Pass-element geometry is emitted in
  physical pixels and scaled from a logical layout; that is correct for
  `transform = 0` (including all HiDPI scales, which are tested) but the
  transform maths for rotated outputs has not been exercised.
* **The overlay is drawn on one monitor** — whichever holds the pointer. Other
  monitors keep showing their normal desktop. There is no per-monitor overview.
* **Windows on hidden workspaces show their last frame briefly.** They are
  un-suspended when the overview opens, but a client needs a frame or two to
  redraw, so the first moments can show stale content for those tiles.
* **`.desktop` and icon-theme scanning happens once, on first use.** Apps
  installed while the session is running are not picked up until the plugin is
  reloaded. The scan walks the icon theme directories directly rather than
  parsing `index.theme`, so on a very large theme the first switcher open can be
  briefly slow; results are cached from then on.
* **Windows with no resolvable icon get an initial-letter placeholder.** This is
  common for terminals launched with an unusual class and for Electron apps that
  do not set `StartupWMClass`.
* **The keyboard grab translates keycodes using the seat's active keyboard
  keymap.** Events injected by a virtual-keyboard client that installs its own
  keymap (`wtype`) may translate to the wrong keysym. Physical keyboards and
  uinput-based tools (`ydotool`) are unaffected.
* **The switcher's ordering comes from Hyprland's window history**, so a
  freshly-started session where nothing has been focused yet falls back to
  compositor window order rather than true most-recently-used.
* **No touchpad gestures.** hyprview has swipe-to-open; hyprspace does not.
* **No animation on the switcher's selection movement** — the highlight jumps
  between entries rather than sliding.
