# Spaces visual design

Reviewed against the working overview and private compositor captures on
2026-10-22. The design keeps the existing Super+A overview as the entry point.
The Spaces drawer normally stays closed; switching context is an occasional
action inside the overview.

## References and interpretation

These are design references, not claims that Hyprspace shares their underlying
implementation.

| Primary source | Relevant pattern | Hyprspace decision |
|---|---|---|
| [Linear: A calmer interface for a product in motion](https://linear.app/now/behind-the-latest-design-refresh) | Quieter navigation, smaller icons, restrained separators, and semantic theme tokens | Keep the workspace previews prominent; use compact identity icons, neutral names, and soft surface boundaries. |
| [Raycast: List](https://developers.raycast.com/api-reference/user-interface/list) | Consistent icon, title, secondary metadata, and keyboard selection | Align space names and workspace/window counts in predictable rows. Keep keyboard focus separate from the current context. |
| [Raycast: Settings](https://manual.raycast.com/settings) | Explicit appearance choices alongside an automatic option | Provide a small theme setting whose automatic behavior is defined by Hyprspace's own configuration. |
| [Notion: Switch between workspaces](https://www.notion.com/en-gb/help/create-delete-and-switch-workspaces) | The current workspace name opens its context list | Use the current icon, neutral name, and chevron as one clear switcher trigger. |
| [GNOME HIG: Popovers](https://developer.gnome.org/hig/patterns/containers/popovers.html) | A compact, button-attached surface for a short list; Esc closes it | Anchor the drawer below the overview header and size it to the visible spaces instead of stretching it to the monitor height. |
| [GNOME HIG: UI styling](https://developer.gnome.org/hig/guidelines/ui-styling.html) | Theme-aware semantic colors and state information conveyed beyond color alone | Derive colors from the configured overview, correct contrast, and show a checkmark for the current space. |

The layout and measurements below are Hyprspace-specific interpretations of
those references, informed by its existing overview and native rendering.
Navigation and text-editing modes stay implicit. The corner guide appears while
a command prefix is pending and disappears when it completes or is cancelled.

## Implemented structure

- The drawer is 304 logical pixels wide when room permits. Its height follows
  the number of visible 44-pixel rows and is capped by the usable monitor area.
  Long lists keep the keyboard selection in view.
- The current icon, neutral name, and chevron form one header trigger. There is
  no adjacent Spaces button or repeated heading inside the chooser.
- The current space has a neutral checkmark. The focused row has a neutral
  background, so moving with the arrow keys does not imply an immediate
  context switch. Enter switches; Esc closes.
- Each row uses a 20-pixel Lucide identity icon, neutral name, and smaller
  right-aligned window count or Empty label. User color applies to identity
  graphics rather than the name text.
- The compact chooser contains space rows, search and small keyboard hints.
  Creation and the full manager open through Space a/p in the corner
  command guide. Escape and outside clicks retain their existing behavior.
- The 432-pixel editor groups Name, Icon, and Color with consistent spacing.
  Selection uses neutral filled icon tiles and focused-field cues.
  The 48 searchable Lucide icons, eight colors, persistence, and editing controls
  remain the appearance model.

## Theme behavior

`plugin:hyprspace:spaces:theme` accepts `auto`, `dark`, or `light`. `auto` derives
the mode from `overview:bg_color`; it does **not** query the operating system's
appearance preference. Overrides affect the control surfaces. Text that floats
over the overview, including the current space heading and empty state, keeps
contrast against the configured overview background.

`SpaceStyle.hpp` provides opaque semantic surface, text, metadata, hover,
selection, focus, and action colors for Spaces, Help, window-board controls, and
the corner command guide. Control surfaces follow the overview background's
hue, balanced with a neutral anchor in both themes; window-title colors do not
tint them. Focused rows use a faint accent wash, and header controls use
translucent fills so the desktop remains visible. Configured label and action
colors are contrast-corrected. Text targets 7:1 contrast in the
standard themes; secondary text has a 4.5:1 floor. Mid-gray custom backgrounds use the best available
foreground contrast, with a 4.5:1 floor. Icon strokes and field focus rings target
3:1. Pastel identity colors are darkened for light surfaces while the stored
color and swatch remain unchanged.

A complete light overview also needs light values for the existing overview
background, title, label, and tile options. The private visual fixtures exercise
both complete overview palettes. Contrast checks live in
`test/test_space_style.hpp`; compositor evidence is recorded in
[Spaces drawer verification](../verification/2026-10-22-spaces-drawer.md).

## Rendering boundary

Feature renderers submit solid fills and outlines in logical coordinates through
`renderPass::appendSolidFill` and `renderPass::appendOutline`. The shared adapter
in `PassElements.cpp` snaps both endpoints to physical pixels and keeps outline
widths in the logical units expected by the compositor. Paired fills and outlines
therefore use the same bounds at fractional output scales. Window-preview
textures retain their separate fractional sampling path. Window-board captions
use the full rounded card shape clipped to the caption band, so their fills stay
inside the card corners even when the control and overview themes differ.

`test/test_render_boundary.py` checks this source boundary, including new feature
files, and runs with `make test`. Pure geometry tests cover fractional scales,
paired bounds, border-width scaling, and invalid or collapsed geometry. The native
`control-surfaces` integration group checks caption interiors and outside-corner
backdrop pixels under opposite light/dark control themes. The `border-geometry`
group checks all four GPU outline edges at ordinary, fractional, and rotated
output scales.
