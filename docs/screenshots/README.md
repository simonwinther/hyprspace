# Screenshots

These PNGs were captured with `grim` from hyprspace running in a separate
Hyprland 0.56.2 session on 2026-09-05. They show real plugin rendering with
local demo content. The overview images are 1920 x 1080; the switcher image is
a 1060 x 500 region captured around the panel. The images have no added labels
or composited interface elements.

| File | Content |
|---|---|
| [overview.png](overview.png) | Three workspaces with source code, notes and build checks |
| [fullscreen.png](fullscreen.png) | A fullscreen Chromium window, Ghostty with Neovim, and Files on one workspace |
| [switcher.png](switcher.png) | App icons and the selected window title |

The browser displays [demo.html](demo.html), which uses local HTML and CSS
without external assets. The editor displays this repository's source and
configuration. Files displays a temporary folder containing those demo files.

## Capture replacements

Use an isolated Hyprland session with the plugin loaded and a 1920 x 1080
output at scale 1. Populate it with demo files and a browser profile that has no
personal accounts. Keep the plugin's default colors, spacing and font sizes.
The sample session used 18 px desktop gaps, 2 px borders and 10 px rounding.

Run these commands from a terminal belonging to that session:

```bash
hyprctl dispatch hyprspace:overview on
sleep 1
grim -o WAYLAND-1 overview.png
hyprctl dispatch hyprspace:close
```

Use the output name reported by `hyprctl monitors`. For the fullscreen image,
first make the browser fullscreen on a workspace with other windows, wait for
its fullscreen notification to disappear, then open the overview. For the
switcher, run `hyprctl dispatch hyprspace:switch` and capture its region with
`grim -g '430,290 1060x500' switcher.png`. Dismiss it with `hyprspace:close`.

Check each image for private titles, notifications and paths before replacing
the repository copies. Keep the original PNG pixels and update this file when
the capture environment changes.
