# Configuration and reference

[Back to the README](../README.md)

## Build and update options

`BUILD_DIR=/some/build/path make` puts build output in a separate directory.
`PREFIX=/some/where make install` changes the installation prefix, and
`PLUGIN_DIR=/some/directory make install` sets the plugin directory directly.
Use the same variables when running `make reload` or `make uninstall`.

Builds track compiler flags, dependency versions and system headers. A Hyprland
update therefore triggers recompilation without a manual clean. `make install`
checks the embedded ABI against the running session. With no active session it
skips the runtime check with a warning; the plugin checks its ABI when loaded.
An unreachable session that is expected to be running stops installation.

`make reload` stages and checks the new binary, unloads the old plugin, then
replaces and loads it. A failed unload stops the operation. If loading fails
and no plugin remains registered, the script restores the previous installed
binary and reloads it only if its ABI can be verified. Concurrent reloads are
rejected. These checks do not make arbitrary plugin runtime changes crash free.

`make install` leaves an already loaded plugin running its existing code until
the next login or explicit reload. `hyprctl reload` rereads configuration but
does not replace a plugin that is already in memory.

## Keybindings

### Hyprland

```ini
# The plugin path must be absolute; Hyprland does not expand `~` here.
plugin = /home/YOU/.local/share/hyprspace/hyprspace.so

bind = SUPER, A, hyprspace:overview
bind = ALT, TAB, hyprspace:switch
bind = ALT SHIFT, TAB, hyprspace:switch, prev
```

`SUPER + A` is the default because it is free on a stock Omarchy and, unlike
`GRAVE`, it is in the same place on every keyboard layout. On a Danish layout
the key above Tab is `½`, so a `GRAVE` binding simply never fires. If you would
rather tap Super on its own, uncomment the `bindr` line in
[`contrib/hyprspace.conf`](../contrib/hyprspace.conf); `bindr` fires on key release, so Super held as a
modifier for another shortcut will not open the overview.

### Omarchy

Omarchy keeps `ALT+SHIFT+Tab` on `changegroupactive`, and an `unbind` in your own
`bindings.conf` will not stick if Omarchy's defaults are sourced after it.
[`contrib/hyprspace.conf`](../contrib/hyprspace.conf) therefore does the unbinds itself:

```ini
unbind = ALT, TAB
unbind = ALT SHIFT, TAB
```

so it only needs to be sourced **after** every other bindings file. Put the two
lines at the very end of `~/.config/hypr/hyprland.conf`:

```ini
plugin = /home/YOU/.local/share/hyprspace/hyprspace.so
source = ~/dev/hyprspace/contrib/hyprspace.conf
```

Never edit the files under `~/.local/share/omarchy/`; they are replaced on
update.

### Dispatchers

| Dispatcher | Argument | Effect |
|---|---|---|
| `hyprspace:overview` | *(none)* | Open on every monitor, or commit the current selection and close |
| `hyprspace:overview` | `on` | Open, never toggle closed |
| `hyprspace:overview` | `off` | Dismiss if open, without selecting |
| `hyprspace:emptyworkspace` | *(none)* | Select an empty workspace on the pointer's monitor; requires an open overview |
| `hyprspace:help` | *(none)* / `toggle` / `on` / `off` | Toggle, open or close searchable help on the selected output; requires an open overview |
| `hyprspace:switch` | *(none)* / `next` | Open the switcher, or step forward |
| `hyprspace:switch` | `prev` / `backward` | Open the switcher, or step backward |
| `hyprspace:close` | *(none)* | Dismiss whichever overlay is up, without selecting |
| `hyprspace:space` | JSON request, or `next` / `prev` | Create, switch, rename or remove a space |
| `hyprspace:sidebar` | `on`, `off`, `toggle` or empty | Toggle the space drawer within an already-open overview |
| `hyprspace:movetospace` | JSON request | Move the selected window or whole workspace into another space |

`hyprspace:close` works over the hyprctl socket even while an overlay holds the
keyboard grab, which makes it a reliable escape hatch.

The overview starts with the focused window and its workspace selected, keeping
the cursor in place. With `follow_mouse = true`, selection follows the mouse
after you move it. Pressing the overview binding again without changing the
selection or moving the cursor returns to the desktop with focus, workspaces
and cursor position preserved.
A session limited to one output starts on that output's active workspace.

### Spaces

Spaces contain independent workspaces. Workspace numbers are local to a space
and shared across its monitors: workspace 1 can exist in both a hobby space and
a work space, with different windows, layouts and monitor placement. The
overview, window board, search and Alt+Tab show the selected space's windows.
Even with `switcher:current_workspace_only = false`, Alt+Tab stays inside that
space. Pinned windows follow workspaces within their owning space; scratchpad
names are local to that space too.

The first launch places existing native workspaces in **Default** without
renumbering them. No other spaces are supplied. Open HyperSpace with the existing
**Super+A** binding, then click the current space name and chevron or press
**Ctrl+Space** or **Space, then s** to open its space drawer. The drawer starts closed, opens below
the current context, and overlays the canvas without moving
workspace previews. Use **j/k**, **Up / Down** and **Enter**, or click a row, to switch
every connected output. Choosing a space closes the drawer and keeps the
workspace overview open. **Escape** or **Ctrl+Space** closes the drawer without
switching. Drawer clicks work while Super is still held.

In Normal mode, **Space, then a** creates a space, **Space, then e** or **F2**
edits the highlighted space, and **Space, then d** or **Delete** opens its
removal preview. **Space p** opens the complete manager. **/** filters
the chooser; Enter or Escape finishes Insert while preserving that filter.
Use **i** to resume typing and **j/k** to choose a filtered space. **Ctrl+Page Up / Ctrl+Page Down** switches spaces directly while the
overview is in Normal mode, including during held zoom. These global commands
are suspended while a form contains a draft. Each space remembers the selected
workspace on each output. A disconnected display's
workspaces use a connected fallback and retain their preferred display for
reconnection. Switching spaces does not change monitor modes or power settings.

The overview heading shows occupied workspace and window counts, excluding
automatically prepared empty workspaces. The drawer shows each space's window
count, or **Empty**. A new space shows **This space is empty** and an
**Enter space** action to close the overview onto its first workspace. Other
spaces stay available through the current space name even when it has no windows.

Open **···** beside the space title for **Edit space**, **Move selection to…**
and **Remove space…**. Removing requires a destination and previews
the workspace transfers before confirmation. Every workspace moves intact;
existing destination workspaces remain separate. Conflicting numbers receive
the lowest free numbers and conflicting names receive a numeric suffix. Default
is a regular space: it can be renamed or removed after another exists. The final
space cannot be removed.

**Space m** opens **Move selected window to space**; **Space Shift+M** opens
**Move workspace to space**. Both commands work from the workspace grid and window
board, and the destination picker starts with the requested transfer type. It
captures the selected window or workspace when opened; if that selection closes,
confirmation is blocked instead of moving something else. The dialog retains its
explicit type switch and leaves the current space selected. The **Move to…** menu
keeps its context-dependent default. **Space p** opens the complete space manager.
In the drawer or Your spaces manager, **Shift+J / Shift+K** moves the selected
space down or up; its position is saved. With a search filter, it moves past the
adjacent matching space. Selection and the current space stay unchanged.
Menus and dialogs support hjkl/arrows, Tab and Enter. Escape in Normal returns
to the parent chooser with its selection and filter retained. A menu keeps
the window or workspace you selected before opening it. Names support Unicode, cursor editing,
Ctrl+A to select all and Ctrl+U to clear. System keys and other native shortcuts
remain available.

Create and Edit offer 48 searchable Lucide icons and eight color swatches. Color changes
the space icon, while the name keeps the normal text color. **Tab** moves between
Name, Icon, Color, Cancel and Save controls. **hjkl** or arrows choose an icon
or color. With Icon focused, **/** or **i** edits the icon search; search by
name or a related word such as “school”, “gaming” or “photography”. Enter or
Escape finishes the search and keeps the filter for navigation. **Page Up /
Page Down** moves through the bounded icon grid; the arrow buttons page it with
the mouse. A search with no results retains the chosen icon. Edit opens in Normal mode; **i** edits Name in Insert. Create starts
in Name Insert. Enter or Escape finishes text editing without losing the draft;
**Ctrl+Enter** saves the entire editor. A second Escape in Normal cancels back
to its parent. Opening help and returning preserves unsaved appearance choices.
Existing spaces receive the grid icon and blue (`#89b4fa`) by default.
Names and appearance can be changed independently without moving workspaces.
Space controls follow the overview's light or dark colors automatically. Set
`plugin:hyprspace:spaces:theme` to `dark` or `light` to override this; `auto` is the
default. Icon colors retain their chosen hue with contrast adjusted for the surface.

Existing numeric workspace and move bindings use local numbers in the active
space; they do not need to be rewritten. Numeric and range workspace rules use
those local numbers, so a rule for workspace 2 applies to workspace 2 in each
space. Native window activation targeting another space switches the complete
desktop to that space. Delayed applications launched through a captured
overview context retain their originating space.

The native Hyprland API still has a global workspace namespace. New backing
workspace IDs and names appear in `hyprctl`. The space drawer and the integrated
Rust workspace slider use local identities and active-space occupancy. The
slider's numbered slots select or create workspaces within the current space;
an empty slot is not occupied merely because that number is used elsewhere.
Other bars and third-party plugins can use the [space-aware bar interface](bars.md)
instead of reading global native IDs directly.

Metadata is stored in `$XDG_STATE_HOME/hyprspace/spaces.json`, falling back to
`~/.local/state/hyprspace/spaces.json`, with atomic replacement and a backup.
Space names, icons, colors, workspace identities and placement survive compositor restarts.
This does not restart applications or restore their application data.

The dispatchers accept JSON; `id` and `space_id` can be a stable ID from status
or the current space name. For example:

```bash
hyprctl dispatch hyprspace:space '{"action":"create","name":"Hobby","icon":"palette","color":"#cba6f7"}'
hyprctl dispatch hyprspace:space '{"action":"create","name":"Work"}'
hyprctl dispatch hyprspace:space '{"action":"switch","id":"Hobby"}'
hyprctl dispatch hyprspace:space '{"action":"rename","id":"Hobby","name":"Personal"}'
hyprctl dispatch hyprspace:movetospace '{"space_id":"Work","target":"window"}'
hyprctl dispatch hyprspace:movetospace '{"space_id":"Work","target":"workspace"}'
hyprctl dispatch hyprspace:space '{"action":"delete","id":"Personal","target":"Work"}'
hyprctl dispatch hyprspace:space next
```

Moves use the overview selection while it is open and native focus otherwise.
Optional `workspace` (native ID) or `window` (native address string) fields select
an explicit source. Use the installed `hyprspace-launch --status` helper to
inspect space-aware status. Lua exposes the same requests as
`hl.plugin.hyprspace.space()` and `hl.plugin.hyprspace.movetospace()`.
The internal drawer dispatcher has the Lua equivalent
`hl.plugin.hyprspace.sidebar("toggle")`, requiring an open overview.

For a Lua configuration, use `hyprctl eval` with the plugin's Lua function:

```bash
hyprctl eval 'local r = hl.plugin.hyprspace.space([[{"action":"create","name":"School","icon":"graduation-cap","color":"#ed8796"}]]); assert(r.ok, r.error)'
```

To opt out, set `plugin:hyprspace:spaces:enabled = false`. The space drawer and
space routing are disabled; native backing workspaces remain accessible.

### Controls

HyperSpace opens in **Normal** mode. `hjkl` and arrows navigate; Space starts a
visible command prefix and `g` starts `gg`. An unknown suffix or Escape cancels
the prefix. Text only changes in **Insert** mode. `/` starts search, `i` resumes
a query or edits the focused Name field, and Enter or Escape finishes editing
without clearing text. A second Enter in Normal accepts a result. Escape in
Normal returns one UI level; at the root it dismisses HyperSpace. An active
gesture is cancelled before navigating back. System shortcuts and foreground
launchers retain native input.

Modes stay implicit. The corner command guide appears while a prefix is pending
and disappears when it completes or is cancelled.

| Normal control | Action |
|---|---|
| `Enter` | Accept the selected workspace, window or chooser item |
| `Space s` / `Ctrl+Space` | Open the spaces chooser |
| `Space p` | Open the complete space manager |
| `Space e` / `Space a` | Edit the highlighted/current space / create a space |
| `Space m` | Move selected window to space |
| `Space Shift+M` | Move workspace to space |
| `Space d` | Remove a space with a transfer preview |
| `Space v` / `Space w` | Show windows / workspaces |
| `Space g` / `Space r` in Windows | Cycle grouping / toggle recent-first order |
| `Space n` / `Space x` | Prepare a workspace / close the selected empty workspace |
| `Space ?` / `?` | Open help |
| `gg` / `G`, Home / End | First / last item |
| `n` / `N` | Next / previous search result |
| `Tab` / `Shift+Tab` | Next / previous item or form field |
| `Ctrl+Page Up` / `Ctrl+Page Down` | Previous / next space across every output |
| Middle-click | Select an empty workspace on this monitor and keep the overview open |
| Hold `Z` | Enlarge the selected workspace; release to smoothly restore the grid |
| `Z` + wheel / vertical two-finger scroll | Magnify around the pointer |
| `Z` + `+` / `−` (`=`, keypad + / −) | Magnify around the viewport center; hold to repeat |
| `Z` + `Space` + pointer motion | Grab and pan magnified content, including one-finger touchpad motion |
| `Z` + `Shift` + arrows | Pan the viewport toward that direction; hold to repeat |
| `Z` + `F` | Enable or disable smooth cursor-follow; the choice is remembered |
| `?` | Open searchable help above the current view |
| `←` `↓` `↑` `→` | Move to the nearest tile in that direction |
| `h/j/k/l` | Same, vim style |
| `1` through `9`, `0` | Go to that workspace at once, no Enter needed (`0` = workspace 10) |
| `Home` / `End` | First / last tile |
| `Page Up` / `Page Down` | Previous / next column in a scrolling workspace; Enter opens the selection |
| Mouse move | Hover highlights, and selects when `follow_mouse` is on |
| Left click | Go there. Clicking a *window* inside a tile focuses that window; clicking empty space dismisses |
| Right click | Close without selecting |
| `Super` + drag left | Rearrange a window or move it to any workspace tile on any output |
| `Super` + drag right | Resize that window in place, scaled into the tile |
| `Super` + `Alt` + drag left | Move a regular workspace to another monitor |
| Wheel / two-finger scroll | Pan the scrolling workspace under the pointer; other layouts step the workspace selection |
| Edge arrows inside a scrolling tile | Reveal the next hidden column and keep the overview open |
| + New workspace | Prepare an empty workspace on this output, or accept a Super-dragged window |

Press **?** in the workspace overview or window board to open an on-the-fly
guide. It shows the shared Normal-mode commands and configured inspection keys, the
workspace-drag modifiers, and the current follow preference.
Native overview bindings are shown when available; Lua callback bindings remain
under your Hyprland configuration.

| Help control | Action |
|---|---|
| `/` | Search every topic; type `keybinds`, `follow`, `resize`, or another term |
| `Enter` while searching | Finish editing and browse the results |
| `j/k` or Up/Down | Choose an entry and read its explanation |
| `h/l` or Left/Right | Choose a topic; clears the search |
| Tab / Shift+Tab | Next / previous entry |
| Home / End, Page Up / Page Down | First / last entry, or a page of entries |
| Wheel or click | Browse entries or choose a topic |
| Ctrl+A / Ctrl+U while searching | Select or clear the query |
| `Esc` | Leave search first, then close help and return to the current view |

The guide preserves the workspace or window selection. Zoom follow and manual
inspection controls pause while it is open; releasing a held zoom key still
returns to the previous view. Printable `?` stays ordinary text in a focused
search field. `overview:help_key` accepts an XKB key name (`question` for `?`)
as an optional alias, or an empty value to disable that alias. The standard
`?` and `Space ?` commands remain available. Inspection keys retain precedence
on a conflicting alias.
The dispatcher also has a Lua entry point, `hl.plugin.hyprspace.help()`.

Hold **Super + Alt** and left-drag anywhere inside a regular workspace tile,
including over window previews or from the enlarged zoom view. Once the motion
exceeds `binds:drag_threshold` in logical pointer coordinates, a carried
workspace card follows the pointer and highlights the destination monitor.
Drop anywhere in another open overview's usable monitor area, including over
tiles or empty space. The workspace keeps its number, name and layout, the
overview stays open and selects its moved tile, and the destination keeps its
current desktop workspace. If the source was showing that workspace, it
switches to another regular workspace. Pinned windows remain on the source
monitor, following Hyprland's native workspace move behavior. On release, the
card and its window previews ease into their destination tile over 180 ms.
The move completes immediately, so the transition does not delay input. The
transition follows Hyprland's global and `windowsMove` animation enable settings.

Only regular numbered and named workspaces can be moved; scratchpads cannot.
Releasing over the source monitor, a panel, a monitor gap or an output without
an overview leaves workspace placement unchanged. Escape cancels the gesture
and keeps the overview open. Starting from zoom returns to the grid when the
gesture finishes or is cancelled. Releasing the modifiers after pickup does
not end the drag. Native keyboard shortcuts retain their usual routing; when
one executes, it cancels the workspace drag first. Window move and resize
controls continue to use Super alone.

Set `overview:workspace_drag_modifiers` to choose the exact modifier combination
that must be held on pickup. It accepts case-insensitive Hyprland modifier names
separated by spaces or `+`, and requires Super plus at least one of Alt, Ctrl or
Shift. Unknown names are rejected. An empty value disables the gesture.

### Experimental window board

Press **Space, then v** while the overview is open to replace its workspace tiles with a
window board on the currently selected output. It gathers windows from every
output covered by the overview. Other outputs remain dimmed. The usual overview
still opens first; **Space, then w** returns to it.

Press **/** directly from the workspace overview to open All windows with the
search field focused. Type an app name, a window title, or both: `chrome project`
finds Chrome windows whose titles contain “project”. Enter or Escape finishes
Insert; Enter in Normal, or a click, focuses the selected window on its original
workspace and monitor.

| Control | Action |
|---|---|
| Toolbar / Space g | Choose All windows, Apps, Workspaces or Monitors |
| Space r / Recent | Switch between location order and recent-first order within each group |
| / / search field | Search titles, app names/classes, workspace names/numbers and monitor names |
| Backspace / Delete / Ctrl+U | Delete before / after the text cursor, or clear the query |
| Left / Right, Home / End while searching | Move the text cursor; hold Shift to select text |
| Ctrl+A while searching | Select the query; typing replaces the selection |
| Arrows, h/j/k/l, Tab / Shift+Tab | Select a window; navigation scrolls it into view |
| Home / End | Select the first / last result |
| Wheel, two-finger scroll, Page Up / Page Down | Scroll the window board |
| Enter in Normal, or click | Focus the selected source window and close the overview |
| Hold Z | Fit the selected window; the existing 1–4× zoom and pan controls apply |
| Z + F | Enable or disable persistent cursor-follow |
| Escape | Leave Insert with the query retained; in Normal, restore the prior view |
| Space n / middle-click | Return to workspaces and prepare an empty destination |
| 1–9 / 0 | Return to workspaces and open that workspace |

The / shortcut follows your keyboard layout, including **Shift+7** on a Danish
keyboard. Holding the opening key does not add text to the query.

The search field has a blinking caret in Insert. Click to position it; long
queries scroll inside the field. Enter or Escape returns to Normal without
clearing the query; hjkl and Tab then browse results. **i** resumes editing.

While search has focus, all printable keys become literal text, including G,
R, S, Z, N, spaces and digits. Search ignores case, supports Danish characters,
and requires every space-separated term to match somewhere in the window's
metadata. Enter with no matches keeps the board open. Modified Hyprland
shortcuts, except the search editing keys above, still act on the selected native
window, including windows on another output. Foreground launchers keep their own
keyboard input.

App groups use desktop-file identity where available and normalized window
class otherwise. Workspace groups also include monitor identity. Each card
shows its title and native location; fullscreen/maximized badges describe the
source window. Grouping never moves windows or changes layout/fullscreen state.
Mapped windows that a crowded native layout has collapsed to zero size remain
searchable and selectable with an icon placeholder.
Window move, resize and workspace drag gestures belong to the workspace view.
Changing groups animates the cards, retains selection and scrolls rather than
shrinking previews below their readable minimum. Titles and window membership
update while the board is open. Losing its display output relocates the board
to another covered output.

Wheel and touchpad scrolling keep your position when titles or window membership
change. Hovering a partially visible card selects it without scrolling; keyboard
navigation reveals the selected card. Scrolling during a grouping animation moves
the cards without interrupting their rearrangement.

Recent order is captured when entering the board. Hovering and navigation do
not reorder it. Group and order preferences survive view changes during one open
overview; a new overview starts with All windows and location order. Search is
retained in the UI return stack when opening a child view or another search.
Closing the overview starts a new browsing session.

`overview:window_view_key` is retained as a deprecated compatibility option.
Use `Space v` / `Space w`; this option does not remap the shared commands.
`hyprspace:windowview` accepts `toggle` (the default), `off`, `flat`, `app`,
`workspace` or `monitor`, and requires an open overview. In Lua use
`hl.plugin.hyprspace.windowview("app")` with the same arguments. For example:

```ini
bind = SUPER, G, hyprspace:windowview, app
```

### Workspace destinations

Press **Space, then x** on a selected empty workspace to close it while keeping the overview
open. If it is active, the nearest remaining normal workspace on the same monitor
becomes active. Closing an inactive workspace keeps the current desktop active.
Workspaces containing windows, special workspaces, and the last normal workspace
on each monitor are protected. The command acts once. Configured persistent
workspaces can return after reloading your configuration.

`overview:close_workspace_key` is retained as a deprecated compatibility option;
it does not remap the `Space x` command.
The same action is available as `hyprspace:closeworkspace` or
`hl.plugin.hyprspace.closeworkspace()` in Lua.

Middle-click without modifiers, press **Space, then n**, or click **+ New workspace** on a
monitor to prepare an empty destination there. This uses the pointer's monitor
even when keyboard navigation selected a tile on another output or `follow_mouse = false`.
It reuses a prepared or active empty normal workspace first, then another empty
normal workspace on that monitor. Otherwise it creates the lowest available
positive workspace number, respecting workspace
monitor rules and workspaces already owned by other outputs. Empty named normal
workspaces can be reused; new workspaces receive numeric IDs. Special workspaces
are not candidates. Conditional monitor assignment rules that cannot
be resolved safely prevent fresh allocation and return an error; reuse remains
available.

The destination is selected and highlighted while the overview stays open.
Workspace tiles stay in numeric order, and preparation does not move the cursor.
Preparing it leaves the active desktop and focus unchanged. Press Enter to open
it, launch an application there, or Super-drag a window into its tile. You can
drop directly onto **+ New workspace** to prepare and move in one gesture;
allocation happens only on a valid release. Escape cancels a provisional drag,
and dismissing an unused prepared workspace releases it without changing your
desktop. Existing persistent workspace settings are retained.

The button hides during held zoom and panning, and reappears for window move
drags, including ones started from zoom. It is unavailable during resizing.
`Space n` or middle-clicking returns to the grid; while a window drag, pan or
released resize is in progress, these shortcuts do nothing. The keyboard command acts once.
Foreground launcher typing, modified native bindings and typing outside the overview
keep their normal behavior.

Middle-clicks on panels or foreground UI and while the overview is closed keep
their normal routing. Middle-clicking an output without an overview, or a monitor gap,
does not prepare a workspace. One middle-button press acts once until release;
its release remains consumed if the overview closes while the button is held.

`overview:empty_workspace_key` is a deprecated compatibility option; `Space n`
is the shared command. The button and `hyprspace:emptyworkspace` dispatcher
remain available. For a custom native binding, call that dispatcher
without arguments; in Lua use `hl.plugin.hyprspace.emptyworkspace()`. Both require
an open overview.

Hold **Z** without modifiers to fit the selected workspace into most of its
monitor, keeping the existing padding and workspace label. Arrows, Tab and
Shift+Tab smoothly browse that monitor's workspaces while Z remains held.
Moving within the enlarged workspace leaves it selected. With `follow_mouse`
enabled, hovering for 250 ms within 48 logical pixels of a usable monitor edge
smoothly enlarges the same neighbor as that direction's arrow key. Edge
hints show available neighbors without reducing zoom, including above and below.
Each entry moves once: leave and re-enter the edge area to browse again. Corners
do not navigate, and crossing onto another monitor retains the zoom destination.
Entering an edge during an opening or camera animation starts the dwell once
the animation settles. Drags and foreground input handoff cancel unfinished
hovers; moving within the edge can start a fresh dwell after input returns.
Scrolling arrows remain separate from workspace navigation, and scrolling layouts
still accept horizontal-wheel and wheel-tilt panning.

While Z is held, the vertical mouse wheel or two-finger scrolling zooms around
the pointer in every layout. Input takes effect during the initial fit or opening animation,
retargeting the displayed camera smoothly. Wheel up enlarges and wheel down
reduces magnification, honoring the device's scroll direction and factor. Extra
zoom is bounded between the normal Z-held fit and four times that fit; scrolling
outward restores its exact framing. High-resolution wheels and touchpad deltas
retain fractional steps. Inspection stays on the held
workspace's output, and wheel input over another output is consumed without
changing that workspace. For keyboard-only zoom, press **+** or **−** while Z is
held; **=**, **Shift+plus** and keypad add/subtract are also accepted. Each press
changes magnification by a factor of 1.15 around the viewport center, even if the
pointer is on another output. Held keys use the keyboard's repeat delay and rate.

Holding **Z** leaves the cursor under your control. A quick press, mouse move and
release keeps its screen position; repeated stationary taps do not move it.
After magnifying with the wheel or **+**/**−**, releasing **Z** carries the cursor
back to the corresponding point in the grid. Movement during that return adjusts
the local point, so you can keep moving while the view shrinks.

On a laptop, hold **Z+Space** and move one finger on the touchpad to pan magnified
content. Space is consumed while Z is held, including at the normal fit where
there is nothing to pan. Release Space to retain the camera. **Z+Shift+arrows**
pans the viewport toward that direction by 40 logical pixels per step, repeating
while held. Unmodified arrows and Tab continue to browse workspaces.

Press **F while Z is held** to toggle cursor-follow in either workspace zoom or
window-board window inspection. Moving right reveals more content on the right;
moving down reveals more below. Movement under four logical pixels is ignored,
and larger movement gently steers the camera within the existing bounds. When
you stop moving, the camera finishes settling. The **F · Follow enabled/disabled** hint
shows the current mode. In workspace zoom it stays in the upper-left corner;
window-board includes it in the inspection hints. Turning follow on waits for fresh mouse movement;
turning it off stops follow at the displayed framing.

While Follow is enabled and Z is held with extra magnification above the fitted
1× view, the cursor stays inside the monitor showing the inspection. This covers
every edge and corner, including during right-drag or Space panning, so you can
reach the pan bounds without accidentally moving to another monitor. Returning
to 1×, releasing Z or disabling Follow restores free movement immediately,
including during the zoom-out animation. If the cursor is already on another
monitor, it stays there until you move it into the inspected output. Foreground
interfaces and window or workspace dragging retain their usual input.

Set `overview:follow_key` to another XKB key name, or an empty value to disable
the shortcut. Set `overview:show_follow_hint = false` to hide the hint in both
workspace and window-board inspection without changing your saved follow preference.

Follow starts disabled until you enable it with F. Your choice lasts through
Z releases, view changes, closing and reopening, plugin reloads and Hyprland
restarts. Only pending camera movement resets; a new inspection waits for fresh
mouse movement. The preference is saved in
`$XDG_STATE_HOME/hyprspace/cursor-follow`, falling back to
`~/.local/state/hyprspace/cursor-follow` under the
[XDG state-directory rules](https://specifications.freedesktop.org/basedir/latest/).
Missing or invalid saved state starts disabled. Follow only moves content above
the normal held fit and works independently of selection's `follow_mouse`
setting. Follow covers the whole inspection output, including its corners and
the reserved Waybar area; moving beyond the fitted footprint reaches the pan
bound, and moving inward steers it back. While magnified follow is active, the
overview retains pointer ownership over ordinary Waybar surfaces. Disabling
follow or releasing zoom restores native panel input. Menus and other foreground
interfaces still receive input. Follow accepts motion during zoom and opening
animations, including while a zoom key is repeating. Wheel bursts keep pending
follow movement without changing the requested magnification. Manual panning,
drags, foreground input and movement outside the inspection output pause follow;
fresh movement resumes it afterward. Wheel and keyboard zoom retain their usual anchors. Zoom,
empty-workspace and window-view shortcuts take precedence if they share the
follow key. `overview:wheel_zoom = false` also disables cursor-follow.

Hold the right mouse button while Z is held to grab and pan a magnified
workspace. The picture follows the mouse, bounded to keep its normal fitted
footprint covered. The open-hand cursor indicates that panning is available;
the closed hand indicates an active grab. Pickup preserves the displayed camera,
including during wheel animation. A grab begun before the initial fit is ready
waits for a movable camera. At the normal fit, there is nothing to pan. Release
the right button to retain the view, or release Z to return to the grid. Wheel
input, keyboard magnification and other mouse actions pause during the grab. Super+right-button resize
remains available when starting outside a grab; ordinary right-click dismissal
remains available outside held zoom.
Edge-hover browsing pauses during extra zoom and its return to the fitted view;
real pointer motion can start a fresh dwell afterward. Keyboard workspace
browsing clears extra zoom and fits the new workspace. Changes to the usable
monitor area or workspace tile also clear extra zoom and refit. Set
`overview:wheel_zoom` to `false` to disable extra wheel, touchpad and keyboard
magnification and panning, retaining the previous scrolling behavior.

Wheel selection of other workspaces pauses during zoom. Release Z at any
magnification to return directly to the overview grid
through the reverse animation. Pointer movement immediately resumes selection,
including during zoom-out, so pressing Z again enlarges the workspace currently
under the pointer. Releasing Z without moving retains the selected workspace.

Zoom affects previews and keeps the overview open. Clicking a window or pressing
Enter in Normal or Super+A again opens the selection directly. Escape cancels
an active gesture before returning through UI levels. Space starts the command
prefix when Z is released. Explicit `off` and
`hyprspace:close` dismiss without selecting. An
output already showing a single workspace keeps its existing Z-fit geometry but
still accepts extra magnification. New zoom
presses and workspace browsing pause during a drag; releasing Z during a drag
still restores the grid, with resize gestures retaining their pickup scale.
Foreground keyboard handoff cancels held zoom and requires a new press afterward.

Scrolling previews accept horizontal and vertical wheel/trackpad input. Wheel
detents move a quarter viewport and high-resolution detents retain their fraction;
touchpad movement is continuous. Native scroll factors and each workspace's layout
direction apply. Edge arrows appear only where columns extend beyond the preview.
Page Up moves left/up and Page Down moves right/down, including reversed layouts.
Tab and Shift+Tab still select workspaces. Clicking a window always focuses it
and closes the overview, including a partially visible window.

![Scrolling columns with edge arrows and a keyboard navigation hint](screenshots/scrolling-controls.png)

Screenshot, volume, brightness and media keys are passed through to the system
while either overlay is up. When `Print` opens an interactive screenshot picker,
hyprspace temporarily yields its input grab and draws underneath that picker;
after the capture or cancellation it resumes automatically. The captured image
still contains the overview or switcher. While the switcher has Alt held,
`Alt+Print` deliberately invokes the configured *plain* `Print` binding instead;
on Omarchy this takes a screenshot rather than opening its screen-recorder menu.

The overview binding toggles: pressing it again closes. Unmodified navigation
and Shift+Tab belong to the overview. Other keys run through Hyprland's native
binding matcher with the indicated window or workspace established first.
Ctrl/Super combinations, repeat/release bindings, submaps and keyboard-specific
binding settings retain their native matching behavior. Unbound keys and
application modifier events are suppressed while the overview owns input.
Super+L uses `hyprspace:layoutcycle`
from the example configuration to select dwindle or scrolling synchronously.
Launching, moving, resizing and changing layouts keep the overview open.

Overview resizing stays on the source workspace. Floating windows stop at its
usable edges, accounting for panels, decorations and application size limits.
Resize previews stay inside the tile even during the opening animation. Releasing
over a gap or another monitor finishes the resize in place; Escape cancels it.

Walker and other foreground layers retain keyboard focus while pointer movement
outside their input regions updates the overview target. See the
[interactive overview and companion integration](interactive.md) for launch
correlation, compatibility and verification details.

When a workspace has a fullscreen or maximized window and other windows, its
previews spread into separate rows inside the tile. Every preview keeps its
aspect ratio and remains individually clickable. The enlarged window has an
outline and a "fullscreen" or "maximized" badge; a workspace with just that
window uses the whole tile. Workspaces without an enlarged window retain their
desktop arrangement.

Opening the overview changes only the previews' positions. Escape returns to
the original desktop, and clicking the fullscreen preview keeps it fullscreen.
Clicking another preview follows Hyprland's fullscreen focus behavior: it can
transfer fullscreen to that window or reveal it at its normal size. If the focus
policy would redirect the click back to the covering window, the overview exits
that window's fullscreen mode and focuses the clicked window.

During the zoom, every preview on the opening or closing workspace moves between
its overview position and its actual desktop bounds. Fullscreen clipping follows
the same animation, including the area reserved by panels. Hover outlines, clicks
and drag pickup use the displayed bounds, and resizing uses the picked-up
preview's scale. Valid drops run Hyprland's native drag lifecycle, including its
fullscreen transitions and layout-specific placement rules. Escape cancels a
provisional drag; releasing between tiles cancels the move.

Workspace 10's tile is labelled **0**, because `0` is the key that goes there,
both here and in Hyprland's own `workspace` binds.

**Switcher**

| Key | Action |
|---|---|
| Hold `Alt`, tap `Tab` | Step forward through most-recently-used order |
| `Alt+Shift+Tab` | Step backward |
| Release `Alt` | Commit |
| `Esc` | Cancel, keep the current focus |
| `Enter` | Commit without waiting for the Alt release |
| `W` (while holding `Alt`) | Close the selected window without focusing it; keep switching |
| `Print` (while holding `Alt`) | Run the plain `Print` screenshot binding, not the system `Alt+Print` action |
| `←` / `→` | Step backward / forward |
| `↑` / `↓` | Move to the nearest icon in the row above / below |
| `Home` / `End` | Select the first / last window |
| `Page Up` / `Page Down` | Move backward / forward by one page |
| Mouse move, click | Hover selects when `follow_mouse` is on; click commits the pointed-at window |
| Scroll wheel | Step through the list |

Other unbound keys are swallowed while the switcher owns input.

When stepping tiles or switcher entries, high-resolution wheels accumulate
partial detents before moving the selection; touchpad and continuous scrolling
advance in controlled steps. Each overlay
keeps its own scroll state, and a new gesture starts without leftover movement.

Large switcher lists stay within the screen and show a small page counter when
needed. Tab and the arrow keys keep the selection visible across pages. The
panel keeps its width while live window titles change, so icons stay put under
the pointer.

Decoded icons are reused between openings, up to 128 icons and 16 MiB of pixel
buffers in memory. Configuration reloads and plugin unloads discard that cache.
GPU textures are still released when the last overlay closes, and icons on
other switcher pages are only uploaded when shown.

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
            bg_dim  = 0.80
            padding = 56
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
| `diagnostics` | bool | `false` | Keep up to 64 recent compositor timing samples in the local status socket |
| `warp_cursor` | bool | `true` | Warp the pointer onto the chosen window when committing |
| `spaces:enabled` | bool | `true` | Enable independent spaces, local workspace numbers and the space drawer |
| `spaces:theme` | string | `auto` | Space controls palette: `auto`, `dark` or `light` |

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
| `overview:tile_bg_color` | color | `rgba(0d0d14d9)` | Plate drawn behind each workspace tile |
| `overview:tile_border_color` | color | `rgba(ffffff1a)` | Hairline around every tile |
| `overview:padding` | int | `56` | Outer padding |
| `overview:gap` | int | `28` | Gap between workspace tiles |
| `overview:preview_filter` | string | `sharp` | Thumbnail sampling: `linear` for the original renderer, `area` for footprint averaging, `sharp` for averaging with restrained sharpening, or `linear-sharp` for sharpening the original scaling. Full-size previews and size-changing animations use the original capture |
| `overview:zoom_key` | string | `z` | Unmodified hold-to-zoom key; empty disables it. Use an XKB key name that does not conflict with overview navigation or system keys |
| `overview:empty_workspace_key` | string | `n` | Deprecated compatibility option; use `Space n`. Does not remap leader commands |
| `overview:window_view_key` | string | `g` | Deprecated compatibility option; use `Space v` / `Space w`. Does not remap leader commands |
| `overview:follow_key` | string | `f` | Toggle persistent cursor-follow while the zoom key is held. Empty disables the shortcut; zoom takes precedence. Navigation, system, zoom and pan keys cannot be assigned |
| `overview:close_workspace_key` | string | `q` | Deprecated compatibility option; use `Space x`. Does not remap leader commands |
| `overview:show_follow_hint` | bool | `true` | Show the follow shortcut and enabled/disabled state while inspecting. Hiding it does not change the saved preference |
| `overview:help_key` | string | `question` | Optional help alias. Empty disables the alias; `?` and `Space ?` remain available. Navigation, modifier, system and inspection keys cannot be assigned |
| `overview:workspace_drag_modifiers` | string | `SUPER ALT` | Exact modifiers for left-dragging a regular workspace to another monitor; requires Super plus Alt, Ctrl or Shift. Empty disables it |
| `overview:wheel_zoom` | bool | `true` | Extra wheel, vertical touchpad and keyboard magnification/panning while the zoom key is held; bounds are 1–4 times the normal held fit. Disable to retain previous scrolling behavior |
| `overview:band_gap` | int | `28` | Deprecated compatibility key; ignored. `overview:gap` controls both axes |
| `overview:all_workspaces` | bool | `true` | Deprecated compatibility key; ignored. Populated, active and persistent workspaces are always included |
| `overview:rounding` | int | `14` | Tile corner radius |
| `overview:border_size` | int | `3` | Selection border thickness |
| `overview:active_border` | color | `rgba(89b4faff)` | Selected tile border |
| `overview:hover_border` | color | `rgba(89b4fa80)` | Hovered tile border |
| `overview:workspace_labels` | bool | `true` | Workspace name under each tile |
| `overview:label_color` | color | `rgba(cdd6f4ff)` | Label colour |
| `overview:title_bg_color` | color | `rgba(1e1e2ee6)` | Backdrop behind that label |
| `overview:include_special` | bool | `true` | Include special/scratchpad workspaces |
| `overview:exclude_workspaces` | string | empty | Comma-separated exact workspace names to omit from the overview and window board, e.g. `special:pdf-picker,special:file-picker`. Surrounding whitespace and empty entries are ignored. Other special workspaces remain visible when `include_special=true`. |
| `overview:all_monitors` | bool | `true` | Open on every monitor at once rather than only the one under the pointer |
| `overview:font` | string | `Sans 12` | Pango font description |

Empty active workspaces, configured persistent workspaces and destinations
prepared during the overview are included.

#### Multiple monitors

By default one press covers the whole desktop: every monitor enters the
overview together, each tiling its own workspaces, and they leave together too.
Picking a workspace on one screen dismisses the others without changing what
they were showing. A monitor with nothing on it still dims, because one screen
left bright next to the others reads as a bug rather than as emptiness.

Number keys find their workspace across the open monitors. The destination
monitor zooms into that workspace while the others return to their desktops.
The expanding destination covers the other previews throughout the close.
Empty workspaces focus their owning monitor too; monitor rules also apply to
numbers whose workspaces have not been created yet.

One session owns all views and the active drag. Started gestures retain pointer ownership over ordinary panels, and wheel input stays with the gesture. Move and resize cursors restore on release or cancellation. Window motion must exceed the native drag threshold in mapped desktop coordinates; workspace motion uses logical pointer coordinates. Released resize pictures remain visible until the deferred native resize commits; a second gesture waits for that commit. A drag preview follows the
pointer across outputs, including their offsets, scale and rotation. The source
and destination use workspace identities and weak window references. Tile
positions stay fixed during the drag while window membership is reconciled.
The last valid target is retained over gaps and foreground UI for commands;
gaps never become valid drop destinations. With `follow_mouse = true`, pointer
movement resumes selection after keyboard navigation. With `false`, pointer
motion updates hover/drop targets while commands and Enter retain the keyboard
selection.

Workspace drops require an overview on the destination monitor, so
`overview:all_monitors = false` leaves uncovered outputs unavailable. Moving a
workspace changes its runtime placement; configured monitor assignment rules
can place it back on their chosen output when the configuration reloads.

Committing always acts on the monitor you picked from, not on the one Hyprland
still calls focused. That distinction matters here: the overview holds the
input grab, so moving the pointer to another screen does not move focus with
it, and the ordinary current-monitor workspace actions would switch the wrong
display.

Set `overview:all_monitors = false` to start a session on the monitor under the
pointer. It remains one session: a toggle anywhere closes it, and explicit `on`
while it is open leaves the current view in place. Close and reopen on another
output to move the session. Other outputs keep their normal windows visible.

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
| `switcher:current_workspace_only` | bool | `true` | Use the active normal workspace on the target monitor. `false` includes windows across the current space's workspaces and outputs, including special workspaces |
| `switcher:font` | string | `Sans 13` | Pango font description |

The target monitor is the one under the pointer, falling back to the focused
window's output if the pointer is outside all outputs. A workspace with one
window keeps that window selected; an empty workspace opens no switcher. The
scope never expands automatically. Unmapped and hidden windows are excluded.

### Icons and rendering resources

#### Desktop entries

Desktop entries follow XDG directory precedence: the user data directory wins
over system directories for the same desktop ID. Hidden and iconless overrides
also suppress lower-precedence copies; `NoDisplay` entries can still provide
icons for running windows. Nested paths form IDs with `/` replaced by `-`, as in
the [desktop-entry specification](https://specifications.freedesktop.org/desktop-entry/latest/file-naming.html).

Class matching tries the existing exact class and Chromium/web-app candidate
sequence. For each candidate, explicit `StartupWMClass` aliases outrank desktop
IDs, which outrank `Name` and basename guesses. Equal-strength collisions use
XDG directory precedence, then lexical desktop ID. `Exec` is not used for icon
matching. Sorting file paths also makes the specification's otherwise undefined
`foo-bar.desktop` versus `foo/bar.desktop` collision deterministic.

#### Resource limits

Capture allocation failures retain a valid previous image. Without one, a
clipped backing rectangle represents the window until capture succeeds. Failed
allocations wait one second before retrying, even during animated resizing.
Captures larger than 32 MiB are downsampled to fit that budget while their logical size and pointer mapping stay unchanged. Allocation failures use the previous capture or backing rectangle; all captures
together are limited to 256 MiB of RGBA pixels and 256 live textures. Existing full-resolution captures
within those limits retain the normal output scale and rotation behavior.

Text and icon textures share a 16 MiB budget and at most 512 live textures/cache entries.
Least recently used entries are evicted first. Textures referenced by a frame
remain valid and count against the budget until that frame releases them. If
those references fill the budget, new labels/icons are omitted temporarily.
The final overlay's teardown drops cached GPU resources. Decoded icons can
remain in the existing 16 MiB / 128-entry CPU cache; icon path lookups are limited
to 512 entries. Text keys are limited to 4096 UTF-8 bytes, and individual CPU
rasters to 4 MiB and 8192 pixels per dimension.

The private diagnostic `status` response includes `resources` counters for live
and peak pixel bytes, cache entries, hits, misses, evictions, allocation failures
and capture fallbacks, plus successful downsampled frames. Byte counts estimate pixel storage; they exclude driver
metadata. Enable `plugin:hyprspace:diagnostics` to record up to 64 recent frames with preparation, capture, render completion and pending pointer-input timings. Samples clear when disabled. Input timing ends at compositor `RENDER_POST`, before output commit. It measures CPU render submission; GPU completion and display presentation require separate measurements.

The resource integration fixture exercises title/width churn, retained
frame references, failed GL allocation and repeated overlay close/reopen.
Each overview view also includes `zoom_edges`: available directions, destination
workspace IDs, hint bounds in global logical pixels, and pending dwell state.
The `zoom` object includes `extra_factor`, `extra_goal`, `inspection_ready`,
`inspection_transitioning`, and `camera_settled` for wheel magnification and its
input/animation state. Input readiness is immediate; `camera_settled` waits for
opening, fit and inspection animations, including cursor-follow settling. Extra
factors stay within 1–4 while the initial fit is still moving. Release clears the inspection transform before the
base camera returns to the grid, so extra factors read 1 during that return.
The zoom object also reports `pan_held`, `panning`, and `pan_available` for an
armed grab, active camera movement, and an eligible displayed lens. The top-level
`overview_cursor` reports the current cursor override, including `grab` and
`grabbing`; foreground ownership or an external override can replace it.
`zoom.cursor_follow_enabled` reports the remembered preference;
`zoom.cursor_follow_settling` reports pending follow movement in either zoom view.

### Animations

The transitions reuse Hyprland's own animation curves rather than inventing
their own timing, so they match the rest of your desktop:

* overview open/close, held zoom and workspace panning → `windowsMove`
* switcher fade → `fadeIn`

Retune those in your `animations` block to change the feel.

---

## How it works

```
              ┌── Event::bus() input.keyboard.key (cancellable, pre-keybind)
              │   input.mouse.move / .button
   Hyprland ──┤
              │   render.pre        → capture live window textures
              └── render.stage      → POST_WINDOWS: add the overlay pass element
```

* **Live tiles.** On every frame, each visible window is rendered into its own
  offscreen framebuffer with Hyprland's own window renderer in *standalone*
  mode: full alpha, no decorations, and it renders windows that
  sit on hidden workspaces too. Those clients are also un-suspended while the
  overview is up, so they keep producing frames instead of showing a stale one.
  When drawing those captures, the overview reapplies each window's resolved
  opacity and blur policy. Tiled windows reuse Hyprland's cached wallpaper blur
  when its optimizations allow it; floating windows follow its live-blur path.
  Blur-disabled windows stay unblurred. The tile backing fades away during the
  closing zoom, and the destination workspace stays visible through the hand-off.
* **Hiding the real windows.** Rather than painting over the desktop, the
  overview warps every collected window to zero alpha, which makes Hyprland's
  normal pass skip them. The session records and restores each window's previous
  fade alpha once, even if it transfers between outputs. Foreground layer surfaces
  and the compositor cursor render after the overview.
* **Drawing.** Panels and labels use ordinary texture, border and rect pass
  elements. Window previews advertise their blur requirements to Hyprland's
  render pass and use its existing OpenGL texture/blur compositor without custom
  shaders. If blur resources are unavailable, previews retain their opacity and
  draw without blur. Plugin-owned pass elements are removed before unloading.
* **Layout.** One tile per populated, active or persistent workspace, every tile shaped like the
  monitor's *usable* area (the output minus whatever the bar reserved). Mapping
  the full output instead would leave an empty strip along the top of every
  tile where the bar sits, which is the single most obvious way to make this
  look wrong. Each window is drawn at its real relative position, except on
  fullscreen or maximized workspaces, where separate preview slots keep windows
  from covering one another. Drawing and pointer hit testing share the same
  animated bounds. Column counts for workspace tiles are all tried and the one
  producing the largest cell wins, so the grid stays close to square: 5
  workspaces become 3 over 2, a single workspace fills the screen, and 10 still
  come out readable. The trailing row is centred rather than left-aligned.
* **The opening transition.** The workspace you are already on starts at full
  screen and shrinks into its cell while the others fade up in place, so the
  desktop appears to fold into the grid rather than being replaced by it.
* **Icons.** Window class → `.desktop` entry → icon name → icon theme file, with
  Chromium/Edge web-app classes (`chrome-chatgpt.com__-Default`) decoded to their
  underlying site, which is how Omarchy's web apps report themselves. SVG goes
  through librsvg, everything else through gdk-pixbuf. Unresolvable apps get a
  tinted rounded square with their initial rather than a blank slot. Desktop and
  icon-theme discovery runs on a worker and builds one filename index, so the
  first switcher frame never recursively scans the filesystem.

Two load-order details worth knowing if you build on this: Hyprland parses the
config *before* it finishes loading plugins, so `bind = ..., hyprspace:overview`
in that same config is rejected as an invalid dispatcher; the plugin calls
`HyprlandAPI::reloadConfig()` at the end of `pluginInit` so the second pass
registers the bindings. And a plugin is never initialised twice, so that cannot
loop.

One deliberate implementation note: hyprspace needs `IHyprRenderer::renderWindow`,
which is `protected`. It reaches it with the standard-blessed explicit-instantiation
idiom ([temp.spec]/6) rather than the widespread `#define private public` hack,
which is undefined behaviour and, as of GCC 16's libstdc++, no longer even
compiles. See `src/Access.hpp`.

---

## Tests and build checks

```bash
make            # build the plugin
make check      # verify the built .so matches the running Hyprland's ABI
make test       # host-side unit tests and isolated build/reload checks
make -C test asan   # same suite under AddressSanitizer + UBSan
make clean
```

`make test` builds the Hyprland-independent parts (the layout and navigation
maths, `.desktop` parsing, window-class resolution and the cairo/pango
rasteriser) and runs them on the host, no compositor required. It covers aspect
uniform monitor-shaped cells at every workspace count from 1 to 10, the grid
staying inside the padded screen at four landscape display widths and a 1080x1920
portrait display, cells never overlapping, the expected grid shapes (1 fills the
screen, 5 becomes 3 over 2, the last row centred), directional navigation
including edges and out-of-range input, pointer
hit testing, localised/malformed `.desktop` files, Chromium web-app class
decoding, icon size and format preference, text ellipsising and SVG/PNG icon
loading. Mouse-button tests cover opening, closing and yielding an overlay
between press and release, so client drags do not get stuck and dismissing
clicks do not leak their releases to applications.

Additional host tests cover high-resolution wheel detents, smooth scrolling,
direction changes and gesture boundaries; switcher page bounds and selection
visibility on landscape, portrait and small outputs; vertical navigation through
partial rows; and decoded-icon cache eviction and pixel-buffer validation.
Preview tests cover compositor-opacity application, opaque rules, both animation
endpoints, destination-workspace visibility and continuous backing-plate fades.
Fullscreen preview layout tests cover mixed window proportions, one to 32
windows, separate clickable bounds, portrait outputs, tiny tiles and stable
columns during resizing. Animation tests check fullscreen clipping across panel
reservations and the visibility of windows obscured on the desktop.

Build/reload tests use small ELF fixtures and a simulated `hyprctl`, with no
commands sent to the desktop session. They cover atomic replacement with an old
file still open, failed links, incremental builds, changed compiler flags and
system headers, missing ABI metadata, commit and library mismatches, renderer
symbol checks, failed unloads and restoring a previous binary after load errors.

`make check` reads the `.hyprspace.abi` ELF section without loading the plugin,
compares it to the ABI reported by `hyprctl -j version`, and checks imported
renderer symbols against the executable of that running session. The plugin
also re-checks the full ABI string at load time and refuses to initialise on a
mismatch, with a notification telling you to rebuild. Binaries predating the
embedded ABI record must be rebuilt before installation.

The nested integration runner compares every directed pair of three outputs
(including same-workspace drops) against native gestures in dwindle, scrolling
and master. It also exercises foreground input, cursor rendering, asynchronous
launches, exact XDG surface correlation, rules, lock and unload. The runner creates
its own compositor, input devices, application fixtures, config and D-Bus session.
It never loads the development plugin into the host compositor. Test artifacts
include logs, screenshots and JSON results in the printed temporary directory.
Audit regressions cover protocol-level key/modifier isolation, IME handoff,
overlapping bottom panels and popups, launch destinations after output removal,
and visibility when a window leaves a single-monitor overview. Scrolling checks
cover all four directions, wheel and finger input, bounds, arrows, keyboard
selection, inactive workspaces, foreground typing and animated pointer targeting.
Hold-to-zoom checks cover ten-workspace fitting, keyboard and edge-hover browsing,
pointer pinning, dwell cancellation, key release and device ownership, foreground handoff, drag mapping,
scrolling, bounded pointer-anchored wheel inspection, high-resolution input,
right-button panning, cursor restoration, mouse reconnects, resource stability
and closing during transitions. Run them alone with
`make integration-test INTEGRATION_ARGS='--only zoom'`.

Cross-monitor numeric selection, empty destinations, preview stacking and the
desktop handoff have separate rendered-frame checks. Run them with
`make integration-test INTEGRATION_ARGS='--only transitions'`.

Independent-space behavior has an isolated `--only spaces` suite.
`--only unified-input` drives the shared key grammar through native input: gg/G,
Space commands, literal text in Insert, retained queries, parent restoration,
keyboard appearance editing, held-key focus changes and draft protection.
The `--only space-selector` suite exercises
drawer and menu clicks, keyboard selection, icon/color choices, Unicode naming,
space switching, transfer previews, window/workspace moves and compact dialog
geometry, saving screenshots beside its JSON and compositor logs. These suites run in
private compositor sessions, including when invoked through the full suite.

[Verification instructions and release gates](interactive.md#verification)
describe the automated and physical suites. The
[dated verification record](verification/2026-09-06-audit-fixes.md) records the tested builds,
results and limits.

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

The launch interface creates a private socket in this compositor's runtime
directory and removes it on unload. Launch contexts live only in memory. Window
fade alpha and cursor ownership are restored when the overview closes and on
unload. Workspace and window actions use ordinary compositor state; layout-cycle
changes are dynamic and follow Hyprland's normal config reload behavior.

---

## Known limitations

* **The plugin ABI is tied to the exact Hyprland commit.** Every Hyprland
  update requires `make && make install` (or `hyprpm update`). The plugin
  refuses to load on a mismatch rather than crashing your session, so the
  failure mode is "Super does nothing", not a lost session. Built against
  **0.56.2**; earlier and later releases may need source changes, since the
  render, config and event APIs all moved in the 0.55 cycle. For the same
  reason, hyprview and hyprshell's Hyprland-facing code could not be reused
  directly; see [NOTICE.md](../NOTICE.md) for what was taken from each.
* **Compatibility is limited to the tested compositor and companion versions.**
  Nested tests cover portrait rotation and fractional scaling; physical tests
  cover the three-output setup with hardware and software cursors. Other GPU,
  compositor and launcher versions need their own validation.
* **Windows on hidden workspaces show their last frame briefly.** They are
  un-suspended when the overview opens, but a client needs a frame or two to
  redraw, so the first moments can show stale content for those tiles.
* **The grid is sized for up to about ten workspaces.** That is the practical
  ceiling on Omarchy and the layout is tested at every count from 1 to 10; beyond
  that the tiles keep shrinking rather than paginating.
* **`.desktop` and icon-theme discovery happens once per plugin load.** It starts
  asynchronously as the plugin loads; if Alt+Tab wins that race, lightweight
  initial-letter placeholders are shown and replaced when discovery completes.
  Apps installed while the session is running are picked up after a plugin reload.
* **Windows with no resolvable icon get an initial-letter placeholder.** This is
  common for terminals launched with an unusual class and for Electron apps that
  do not set `StartupWMClass`.
* **Uncorrelated launches retain native placement.** If a reused process discards
  activation information, hyprspace does not infer a destination from its app ID,
  PID alone or the next window to appear. Walker result targeting requires the
  [pinned companion patches](../companion/README.md).
* **The switcher's ordering comes from Hyprland's window history**, so a
  freshly-started session where nothing has been focused yet falls back to
  compositor window order rather than true most-recently-used.
* **No swipe-to-open gestures.** Two-finger scrolling can navigate an open
  overlay, but it cannot open one.
* **No animation on the switcher's selection movement**; the highlight jumps
  between entries rather than sliding.
