# Screenshots

The overview, fullscreen and switcher PNGs were captured with `grim` from
hyprspace running in a private background Hyprland 0.56.2 session on 2026-09-08.
They show Neovim and Files with this repository's source and configuration.
The overview images are 1920 x 1080; the switcher image is a 1060 x 500 region captured around the panel.
The images have no added labels or composited interface elements.

| File | Content |
|---|---|
| [overview.png](overview.png) | Three workspaces with source code, bindings and a file browser |
| [fullscreen.png](fullscreen.png) | Fullscreen Neovim in Ghostty alongside Files on one workspace |
| [switcher.png](switcher.png) | App icons and the selected window title |
| [scrolling-controls.png](scrolling-controls.png) | Scrolling columns with edge arrows and the Page Up/Page Down hint |
| [zoom-overview.png](zoom-overview.png) | Four workspaces before inspection |
| [zoom-inspection.png](zoom-inspection.png) | Pointer-anchored wheel zoom with the grab cursor |
| [zoom-pan.png](zoom-pan.png) | Right-button pan with the grabbing cursor |
| [zoom-pan.gif](zoom-pan.gif) | Animated 640 x 360 preview of zooming, panning and returning |
| [zoom-pan.mp4](zoom-pan.mp4) | Full-resolution 1920 x 1080 video of the same sequence |

Files displays a temporary folder containing copies of the project files.
Neovim displays the switcher layout, bindings, regression test and Makefile.

The scrolling image was captured on 2026-09-06 during the isolated Hyprland
0.56.2 integration suite. It keeps the original 960 x 600 pixels and shows
disposable GTK test windows without personal content.

The zoom media was captured on 2026-10-04 in a private background Hyprland
0.56.2 session at scale 1. It shows copied project source in Neovim/Ghostty,
a temporary project folder in Files, and a disposable controls document. The
three PNGs preserve the original 1920 x 1080 pixels. The video was recorded at
30 fps with the cursor included; the GIF is a smaller 10 fps preview. Neither
has added labels or composited interface elements.

The sequence holds Z, scrolls inward around the pointer, pans in both directions
with the right button held, scrolls back to the fitted minimum, then releases Z
to return to the grid. The capture checked both hand cursor states, the exact
minimum and unchanged native window geometry. It used revision `6af06c7` and
plugin SHA-256 `dab94626b8dfc4e6a9efdeeb99b7416f75f388bb8eab269eb6e8d90ab808bb40`.

## Capture replacements

Use an isolated Hyprland session with the plugin loaded and a 1920 x 1080
output at scale 1. Populate it with copies of project files in Neovim and Files.
Keep the plugin's default colors, spacing and font sizes.
The sample session used 18 px desktop gaps, 2 px borders and 10 px rounding.

Run these commands from a terminal belonging to that session:

```bash
hyprctl dispatch hyprspace:overview on
sleep 1
grim -o WAYLAND-1 overview.png
hyprctl dispatch hyprspace:close
```

Use the output name reported by `hyprctl monitors`. For the fullscreen image,
first make Neovim fullscreen on a workspace with other windows, dismiss any
startup notification, then open the overview. For the switcher, run `hyprctl dispatch hyprspace:switch` and capture its region with
`grim -g '430,290 1060x500' switcher.png`. Dismiss it with `hyprspace:close`.

Check each image for private titles, notifications and paths before replacing
the repository copies. Keep the original PNG pixels and update this file when
the capture environment changes.
