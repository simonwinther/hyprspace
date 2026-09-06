# Interactive overview

The overview is one session with a view on each output. Commands resolve to a
workspace identity and an optional weak window reference, plus a desktop point
mapped from the visible preview. Empty active and configured persistent
workspaces are included. Monitor offsets, scale, rotation, reserved areas and
separated fullscreen previews are part of that mapping.

Pointer motion selects a destination. Over a foreground layer or a gap, commands
retain the last valid destination. Dropping in a gap cancels the move. Keyboard
focus navigation selects its resulting window until pointer motion resumes.
An empty part of a tile targets that workspace's last focused eligible window,
or no window if the workspace is empty.

## Input and rendering

Both overlays render at `RENDER_POST_WINDOWS`. Hyprland draws top/overlay layers,
their popups, notifications and the cursor afterward. Cursor ownership is scoped
to overview input and restores the previous overrides on handoff and dismissal.
Normal pointer focus is re-established on close.
Waybar panels configured on the bottom/background layer are also queued through
the native layer renderer above the overview. A scoped native hit-test adapter
promotes those panels during pointer routing, then restores the compositor's layer
lists and fullscreen policy. Popup and grab handling stays native.

Unmodified navigation and Shift+Tab stay with the overview. Other keys pass once
through the real keybinding matcher. Device maps, modifiers, repeats, releases and
submaps stay under native control. The chosen workspace/window is established
before running an action; native focus warps are suppressed during that action.
Launching, moving, resizing and changing layouts keep the overview open.
Application seat keyboard focus is cleared while the overview owns input, while
Hyprland's own focus and binding state continue to update. This suppresses keys,
modifier notifications and focus-enter delivery. IME modifier forwarding is also
suppressed during ownership; native focus and modifiers resume on dismissal,
handoff or unload, respecting lock and grab restrictions.

Wheel and two-finger input over a scrolling workspace move the native tape within
its bounds. Wheel fractions remain proportional; finger deltas map into logical
preview distance. Scroll factors come from the device that emitted the event.
Edge arrows reveal the closest hidden column using native fit/center behavior.
Page Up/Page Down select the previous/next column in screen order, keeping that
window selected until the pointer moves. Enter focuses it and closes. Workspace
direction overrides, reversed layouts and animation frames use the same geometry
for rendering and hit testing. Fullscreen previews remain independently exposed
and do not need viewport scrolling. Other layouts keep their tile wheel navigation.

Walker owns typing, paste, navigation and activation while its layer has keyboard
focus. Moving outside Walker still updates the destination. Layer pointer events
use native routing. The companion restricts Walker's transparent, output-sized
layer input region to its visible panel while the overview is active, restoring
the ordinary region on dismissal or plugin loss. Escape closes Walker first;
closing or crashing the layer restores overview input. Screenshot-selector
handoff and exclusive grabs retain
their existing native routes. Session locking immediately ends the overview.

`contrib/hyprspace.conf` retains Super+A and replaces Super+L's asynchronous shell
layout query with `hyprspace:layoutcycle`. This chooses dwindle or scrolling on the
indicated workspace. Outside the overview it acts on the active workspace.

## Drag lifecycle

Super+left and Super+right create provisional move/resize previews. The session
owns one drag across all outputs, including preview portions crossing output
edges. The destination tile and insertion point are highlighted. Collection uses
stable workspace identities while tile positions remain fixed during a drag.

A valid release replays pickup and drop in Hyprland's native drag controller at
mapped desktop coordinates. Layout algorithms, floating pickup offsets, grouping
and fullscreen transitions use that lifecycle. Resizing flushes its final motion
after one output refresh interval to respect native motion coalescing. A key
release during that interval preserves the committed resize and still runs native
release bindings. Escape,
window disappearance, output removal, closing and unload cancel pending work.

Window fade visibility is saved once per session. Moving between covered views
retains that value; moving outside all covered outputs restores it immediately.
Returning to a covered output hides the window again without replacing the saved
value. Dismissal restores only windows still hidden by the session. Weak references
prevent restoration to destroyed windows or actions on recycled tile indices.

## Launch protocol

The plugin requires Hyprland **0.56.2**, commit
`efb50993780079460b0cbed1363e2166a2de1d9f`, and the exact library ABI embedded by
the existing build checks. Internal hooks are isolated in `CompositorHooks.cpp`
and `Launch.cpp` and restored on unload. Other commits are refused.

The private `SOCK_SEQPACKET` socket is:

```
$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/hyprspace.sock
```

It is mode 0600, checks peer UID, bounds concurrent clients and requests, and
expires contexts after two minutes. Each connection sends one packet:

| Request | Reply |
|---|---|
| `capture` | Opaque one-shot destination token, or empty |
| `consume TOKEN` | Correlated launch/activation token, or empty |
| `active` | `1` while an unlocked overview is active, or empty |
| `status` | Local diagnostic JSON with targets, previews and visibility |

Walker captures at result activation, not when its UI opens. Direct application
bindings capture through the synchronous dispatcher scope. The destination is
frozen before asynchronous execution. The helper installs per-child
`HYPRSPACE_LAUNCH_TOKEN` and `XDG_ACTIVATION_TOKEN`; it never changes compositor,
launcher-service or global environments.

A new process can correlate its mapped window through its launch environment.
For an existing process, the plugin wraps native XDG activation callbacks and
retains native validation, associating the token with the exact requested
surface. Requests before mapping are retained weakly until that surface maps.
Only a correlated new window receives placement. Explicit workspace or monitor
rules win. Reused existing windows always follow native activation.

Applications and service wrappers that discard correlation retain native
placement. The plugin never matches the next arbitrary window, app ID or an
existing process's PID to guess a launch. See the [companion build guide](../companion/README.md).

## Verification

See the [audit-fix verification record](verification/2026-09-06-audit-fixes.md) for
the current build and checks, and the [initial record](verification/2026-09-06.md)
for the companion rollout and remaining scope limits.

Host checks cover mapping, target retention, workspace identities, concurrent
one-shot contexts, cancellation and visibility restoration, alongside the existing
geometry/render-policy checks. Build/reload fixtures and helper process tests run
with `make test`; host sanitizers run with `make -C test asan`.

The integration suite runs in the background by default. It requires Hyprland 0.56.2,
hyprctl, wtype, grim, Python GObject bindings for GTK3/GtkLayerShell, Pillow,
wayland-scanner, wayland-protocols, the `wlroots-0.20` development package and a
working EGL render node. It creates a private display host, Hyprland compositor,
D-Bus session, three outputs and disposable clients. Physical monitors, keyboard
focus and the desktop cursor are not used. A private runtime directory separates
all sockets, and the child compositor cannot acquire a physical seat. The
Hyprland binary, plugin and user configuration on the desktop are not replaced.

```sh
make integration-fixtures
python3 test/integration/run.py --companions build/companions/bin --firefox
```

The private host in `test/integration/headless.c` uses only wlroots' headless
backend. It presents three unoccluded surfaces in memory, so frame callbacks keep
running while the desktop is on another workspace or running a fullscreen game.
It exposes the protocol versions required by Aquamarine 0.14, sends protocol pings
to flush its initial output requests, and accepts Aquamarine's early bootstrap
buffer before the first configure acknowledgement. These accommodations apply to
the render host; the tested Hyprland and plugin binaries remain unchanged.
Protocol and input tests continue to exercise that child compositor.

The runner lowers its CPU priority to nice 10. Rendering still uses GPU time and
memory, so it can affect game performance. For performance measurements, run tests
on another machine. No input or display isolation removes resource contention.

`--visible` opts into the old on-screen mode and requires a parent Hyprland session.
It opens and arranges three windows on the desktop. Background startup failures
never fall back to this mode. `--runtime` accepts only a saved private session with
matching mode metadata; reusing a visible session also requires `--visible`.
Interrupting or terminating the runner stops its owned compositor and display host.
Optional locally unpacked wlroots dependencies can live under
`build/test-tools/wlroots/usr`; their library path applies only to the display-host
fixture. They are not installed into the desktop session.

`--quick` reduces the layout matrix; `--only` selects a suite during debugging.
`--only audit` runs the input/visibility/launch regressions, and `--only scrolling`
runs viewport controls across all directions and outputs. Both are included in
the full suite. The repeat test measures a synchronous native resize, so completion
of an already launched child process cannot be mistaken for a stuck repeat timer.
Drag fixtures explicitly hold their virtual keyboard modifier until mouse release;
they do not rely on a fixed-duration keypress. Layout comparisons seed pointer
position and window mapping order, require identical starting geometry, and reject
off-screen gesture coordinates before comparing results.
The full matrix compares all nine directed source/destination pairs for dwindle,
scrolling and master against native gestures, including portrait rotation,
fractional scaling, negative offsets and gaps. Further suites cover foreground
input/cursor rendering, launch correlation, explicit rules, lock, reload/unload,
floating/fullscreen/maximized/grouped states and the patched Walker UI.
`--firefox` uses a private browser profile and D-Bus session to verify a new window
in an already running Firefox process and native reuse of an existing window.
`--discord /path/to/Discord` checks existing-process reuse with a private Discord
profile. Neither application check signs into an account or uses a personal
profile; application startup still requires its normal runtime services.
The Discord fixture connects to the parent's audio services and opens its main
window through ordinary activation before testing contextual window reuse.
Keyboard checks include Super+A/B/J/L and Super+Shift+A, Ctrl bindings, repeats,
releases, submaps, empty-workspace focus and input restoration. Lifecycle checks
cover empty destinations, workspace transfers and cancellation on window/output
removal. Diagnostics also check for duplicate native layout membership.

Results, screenshots and failure diagnostics remain in the printed temporary
directory. A run without `--companions` does not verify the Walker/Elephant UI.
Host sanitizers cover the host harness, not the compositor's loaded plugin.
The [background verification record](verification/2026-09-06-background-tests.md)
records the tested display host, coverage and cancellation checks.

The opt-in physical suite is `test/integration/physical.py --run` (run with Python
inside a private `dbus-run-session`). It requires the matching plugin already
loaded on three physical outputs. It compares all 27 layout/output pairs using
temporary workspaces, checks both cursor modes on every output, saves screenshots,
exercises wheel, touchpad, arrow and keyboard viewport controls,
and restores active workspaces, focus and cursor settings. It re-reads the user's
configuration to remove temporary workspace rules; run it when transient runtime
configuration can be reloaded. This mode takes over the live desktop and cannot
run unobtrusively while someone uses it. Schedule it separately and only with an
explicit request from the person using the machine. Hardware cursors, physical
hotplug and the installed desktop services still need these checks before release.

The same physical script accepts `--firefox` and `--discord /path/to/Discord` to
run application checks instead of the layout matrix, also inside a private
`dbus-run-session`. The `--launcher` mode instead requires the **current desktop
D-Bus session** to reach the installed Walker, Elephant and notification services.
It uses a temporary desktop entry to verify a delayed launch through the service
launcher, shows and closes its own notification, and cancels the installed Print
screenshot selector with Escape. Its desktop entry and fixture windows are
removed afterward. Run launcher mode separately from the private application
checks.

This work is not released. Before release, all automated suites must pass on the
final binary and companion builds. Separately record a physical three-monitor
pass with hardware and software cursors, the installed Waybar/notification and
screenshot-selector setup, and existing Firefox/Discord processes. Nested
protocol fixtures establish correlation behavior but do not replace those
application-specific and physical-session checks.
