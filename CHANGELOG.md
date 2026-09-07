# Changelog

## 1.0.0 (2026-09-07)


### Features

* **docs:** add documentation and templates for issues and contributions ([e721031](https://github.com/simonwinther/hyprspace/commit/e721031b4d28d9962cbd6747b95b6781022c7bdd))
* **docs:** update README with new screenshot handling instructions ([64aebdb](https://github.com/simonwinther/hyprspace/commit/64aebdbcb9072cc49dfde950765be4462c872599))
* **install:** add tagged installs and Nix package ([5e25df2](https://github.com/simonwinther/hyprspace/commit/5e25df2c31e6c69eeb0ce78c04f53c459fce975d))
* **overview:** make workspaces interactive ([8c36ae7](https://github.com/simonwinther/hyprspace/commit/8c36ae7815a491218d0e6e587974b34f32cb9082))


### Bug Fixes

* **build:** declare JSON header dependency ([4383a73](https://github.com/simonwinther/hyprspace/commit/4383a73a001886c6e2dc20125abc25a97015ebcd))
* **overview:** bound workspace resizing ([1188778](https://github.com/simonwinther/hyprspace/commit/1188778ba08996e43957f0781ebfd3c0f23bd6ba))
* **overview:** resolve interaction audit findings ([8fe2ba5](https://github.com/simonwinther/hyprspace/commit/8fe2ba5c6127a7d193b33302e683967e6e8683a8))

## Changelog

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
