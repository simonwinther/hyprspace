# Attribution

hyprspace was written with reference to two MIT-licensed projects. No code was
copied verbatim. The Hyprland plugin API has moved on from what either project
targets, but the following ideas were taken from them:

  hyprview, https://github.com/yz778/hyprview
  Copyright (c) 2025 yz778
  MIT License

    * Rendering each window into its own offscreen framebuffer with Hyprland's
      own window renderer, then compositing those textures as overview tiles.
    * Driving the open/close transition from a Hyprland animated variable so the
      overview inherits the user's configured animation curve.
    * Restoring the previously focused window when the overview is dismissed
      without a selection.

  hyprshell, https://github.com/H3rmt/hyprshell
  Copyright (c) 2025 Enrico Stemmer
  MIT License

    * The shape of a GNOME-style Alt+Tab switcher: a most-recently-used window
      list, stepping while the modifier is held, committing on modifier release
      and cancelling on Escape.
    * Resolving a window's app icon by matching its class against .desktop
      entries, including the StartupWMClass fallback.

Both projects are MIT licensed; the shared license terms are in [LICENSE](LICENSE).
