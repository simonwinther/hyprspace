# Changelog

## [1.1.0](https://github.com/simonwinther/hyprspace/compare/v1.0.2...v1.1.0) (2026-09-22)


### Features

* **config:** add Lua dispatcher bindings ([21d165d](https://github.com/simonwinther/hyprspace/commit/21d165dc514b9b3d6e3602359c25d04147abe073))


### Bug Fixes

* **config:** align workspace option defaults ([cb4036a](https://github.com/simonwinther/hyprspace/commit/cb4036a7e09b2cdba7b27d30dbf9e728a33c4ffd))
* **hooks:** preserve foreign dispatchers ([658dcdc](https://github.com/simonwinther/hyprspace/commit/658dcdc3262a1573f6430aff7b13c98ff1720a73))
* **icons:** resolve compressed and scaled assets ([6a5fade](https://github.com/simonwinther/hyprspace/commit/6a5fade2145eebee415d65ed39d91c9e135948e9))
* **icons:** respect desktop entry precedence ([43d0cc6](https://github.com/simonwinther/hyprspace/commit/43d0cc6b853e0610e3c36695ff5aa1da8cf70a82))
* **launch:** preserve service command arguments ([665dcaa](https://github.com/simonwinther/hyprspace/commit/665dcaa90ff794b28617d8a4808868b324269643))
* **overlays:** block activation while locked ([d93b9c3](https://github.com/simonwinther/hyprspace/commit/d93b9c36d22ac03ce5a5e4c7968af89ce787a543))
* **overview:** clear stale tiles on empty refresh ([e032e24](https://github.com/simonwinther/hyprspace/commit/e032e24f9d9752d5cf20c5de110f24bda4403a0f))
* **overview:** retain keyboard selection ([08239f6](https://github.com/simonwinther/hyprspace/commit/08239f666d80489deb28f6d32cb942f7f1a8da28))
* **render:** cap capture and texture memory ([838b8b5](https://github.com/simonwinther/hyprspace/commit/838b8b58dbed1f733862b18d4428446fba132163))


### Performance Improvements

* **overview:** reuse unchanged layouts ([48f48a4](https://github.com/simonwinther/hyprspace/commit/48f48a4744b3a04751591f67408cc2687648fbb7))
* **raster:** reuse measured text layouts ([5de62a1](https://github.com/simonwinther/hyprspace/commit/5de62a1378307bf591b4db35a9c6bc88270c3320))

## [1.0.2](https://github.com/simonwinther/hyprspace/compare/v1.0.1...v1.0.2) (2026-09-10)


### Bug Fixes

* **release:** test versions independently ([04422e9](https://github.com/simonwinther/hyprspace/commit/04422e98027bb5d49d66b52c589a16c1ce623dfc))

## [1.0.1](https://github.com/simonwinther/hyprspace/compare/v1.0.0...v1.0.1) (2026-09-07)


### Bug Fixes

* **release:** consolidate initial release notes ([837d07b](https://github.com/simonwinther/hyprspace/commit/837d07bb63232a2e96cf9b3e27bb5539a3aad8fa))
* **render:** show switcher over fullscreen ([ff6775d](https://github.com/simonwinther/hyprspace/commit/ff6775d8638fc32b99347d0ecf37d444ca3d4dd8))

## 1.0.0 (2026-09-07)

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
- One overview session coordinates targeting, visibility and native dragging
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
