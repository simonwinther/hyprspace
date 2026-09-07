# Changelog

## Unreleased

Initial release notes. Move these into the first release entry during review.

- Provide tagged hyprpm and Nix installations with minimal default bindings and
  reviewed source release automation.
- Pan scrolling previews with wheel/trackpad input, reveal hidden columns with
  edge arrows, and select columns with Page Up/Page Down without dismissing.
- Match promoted Waybar panels' input order to rendering, including popups and
  fullscreen overlap. Isolate application and IME modifier delivery while the
  overview owns input and restore native focus on dismissal or unload.
- Preserve launch destinations after their workspace transfers and its previous
  output disconnects. Restore window visibility immediately when a window leaves
  every output covered by a single-monitor overview.
- Finish committed resizing when Super is released before the next frame.
- Add protocol and scrolling regressions and measure native key repeats without
  racing asynchronous child processes.
- One overview session now coordinates targeting, visibility and native dragging
  across outputs. Empty active and persistent workspaces are included.
- Render before foreground layers and cursors, with scoped input/cursor ownership.
- Route ordinary bindings through the native matcher and provide synchronous
  workspace layout cycling.
- Add per-request launch contexts, exact XDG surface correlation and pinned
  Walker/Elephant companion patches with an isolated build workflow.
- Add nested compositor comparisons, foreground/launch/lifecycle tests and
  opt-in physical monitor and application checks.
- Open the overview on every monitor, select a workspace or focus a window.
- See every window on workspaces with a fullscreen or maximized window.
- Move windows between workspace previews and resize them with Super + drag.
- Cycle windows with app icons, commit on Alt release and cancel with Escape.
- Build, install and reload with atomic file replacement and ABI checks.

Tested with Hyprland 0.56.2 (`efb50993780079460b0cbed1363e2166a2de1d9f`).
Build from source against the compositor and libraries installed on your machine.
After a Hyprland update, rebuild the plugin before loading it again.
