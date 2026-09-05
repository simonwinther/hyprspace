# Changelog

## Unreleased

- One overview session now coordinates targeting, visibility and native dragging
  across outputs. Empty active and persistent workspaces are included.
- Render before foreground layers and cursors, with scoped input/cursor ownership.
- Route ordinary bindings through the native matcher and provide synchronous
  workspace layout cycling.
- Add per-request launch contexts, exact XDG surface correlation and pinned
  Walker/Elephant companion patches with an isolated build workflow.
- Add nested compositor comparisons, foreground/launch/lifecycle tests and
  opt-in physical monitor and application checks.

## 1.0.0

hyprspace adds a live workspace overview and an Alt+Tab switcher to Hyprland.

- Open the overview on every monitor, select a workspace or focus a window.
- See every window on workspaces with a fullscreen or maximized window.
- Move windows between workspace previews and resize them with Super + drag.
- Cycle windows with app icons, commit on Alt release and cancel with Escape.
- Build, install and reload with atomic file replacement and ABI checks.

Tested with Hyprland 0.56.2 (`efb50993780079460b0cbed1363e2166a2de1d9f`).
Build from source against the compositor and libraries installed on your machine.
After a Hyprland update, rebuild the plugin before loading it again.
