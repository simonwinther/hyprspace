# Interactive overview audit fixes — 2026-09-06

The five findings in the [audit](../audit-2026-09-06.md) are resolved. The tested
development plugin is installed and loaded on the three-monitor desktop.
No release or version change was made.

## Changes and regressions

| Finding | Fix and verification |
|---|---|
| Bottom-layer Waybar clicks reach hidden windows | Temporarily promote panels during native pointer hit testing and restore their layer state afterward. Protocol tests cover overlapping floating/fullscreen windows and a native popup grab. |
| Application modifier events leak through | Clear application seat focus during overview ownership while retaining compositor focus and binding state. Block IME modifier forwarding during ownership. Protocol tests cover keys, modifiers, focus-enter, native commands, IME, dismissal, unload and lock. |
| Scrolling previews cannot reveal hidden columns | Pan with wheel/trackpad input, show edge arrows, and support Page Up/Page Down followed by Enter. Tests cover four directions on three outputs, bounds, fractional input, fit/center behavior, workspace direction overrides, scroll factors, inactive workspaces, foreground typing and animation. |
| Launch target depends on its previous output | Resolve workspace identity and current monitor before consulting the captured monitor. A delayed application launch succeeds after workspace transfer and disconnection of the original output. |
| A window moved outside a single-monitor overview remains invisible | Release visibility when a window leaves every covered output; reacquire it on return while retaining the original alpha. Repeated transfers and dismissal restore visibility without duplicate layout membership. |

Wheel detents move a quarter viewport and fractional detents remain proportional.
Finger deltas move continuously in preview coordinates, with native device scroll
factors. Page Up moves left/up and Page Down moves right/down. Edge arrows reveal
the next hidden column without closing. Window clicks still focus and close;
Tab/Shift+Tab still navigate workspace tiles. See the [controls](../guide.md#controls).

The final rerun exposed an additional resize issue: releasing Super immediately
after the mouse button ended the native drag before its queued final motion.
The compatibility adapter now preserves committed resizing across key releases,
while still executing native release bindings. Other key presses and normal
cancellation retain native behavior.

## Tested build

Plugin SHA-256:
`a4397140a611ae41db0ac435504c874d3252b6720b1bad0364506b584acd36d0`.

The compositor, libraries, companions and graphics match the
[initial verification matrix](2026-09-06.md#tested-builds): Hyprland 0.56.2 at
`efb50993780079460b0cbed1363e2166a2de1d9f`, Walker 2.17.0 and Elephant 2.22.0
with their repository patches. The installed binary was replaced by guarded,
atomic reload after every nested interaction passed. The original audited binary
is backed up at `/tmp/hyprspace-audit-fixes-rollout.jsbjvcgi/previous.so`.

## Verification

| Check | Result |
|---|---|
| Host assertions, GCC and Clang | 327,265 passed with each compiler |
| Host ASan and UBSan assertions | 327,265 passed |
| Build/reload fixtures | 69 passed |
| Launch-helper tests | 7 passed |
| Release tooling | 8 tests passed; metadata consistent |
| Exact ABI and guarded reload | Passed |
| Nested suite, patched companions, Firefox and Discord | 111 unique checks passed across the full run and a focused rerun |
| Physical layout, resize, scrolling and cursor suite | 40 unique checks passed across the matrix and cursor rerun |
| Installed Walker/Elephant, notifications, screenshot handoff and restoration | 4 passed |

The physical outputs were DVI-D-1 in portrait, HDMI-A-1 and DP-2. Each native
gesture comparison started with equal geometry; wheel/finger events and keyboard
controls were injected through Wayland test devices. Hardware cursor use was
confirmed on all three outputs, followed by software-cursor rendering checks.
The suite restored active workspaces, focus, cursor settings and window visibility.

The final binary completed 105 checks in `/tmp/hs-i._dk2gxkd` before Discord's
context-carrying reinvocation exceeded its 15-second process timeout. A fresh
instance of the same binary passed Discord reuse and all five subsequent
activation/lock checks in `/tmp/hs-i.89mh9t81`. This totals 111 unique checks;
it is not a single uninterrupted passing run. No plugin changes were made
between those runs. The earlier build passed all 111 in one run, but did not
include the immediate-Super-release resize fix.

The physical run in `/tmp/hs-physical.whgsrxe7` passed every layout/output pair,
immediate-release resize and scrolling check. Its cursor test then cropped the
wrong point by recomputing preview geometry after the pointer warp. Both captured
images contained the cursor. The fixture now keeps the original warp coordinate;
all six cursor cases and restoration passed in `/tmp/hs-physical.7ipggkm5`.
The installed launcher run passed in `/tmp/hs-physical.8revu4ah`.

The harness also fixes the audit's asynchronous repeat-marker race by measuring
synchronous native resizing. Startup failures before output-size acknowledgement
receive bounded retries. Physical investigation exposed two further fixture
problems: comparisons could start from different dwindle trees or target clipped
coordinates outside a scrolling viewport, and the 650 ms modifier timer could
expire during a gesture. Fixtures now seed window map order and pointer position,
assert equal starting geometry and on-screen coordinates, and explicitly release
the modifier after the mouse button. Focused physical traces and the final matrix
verify the resulting comparisons.

The physical screenshot check waits for all selector layers to map and allows
150 ms for client initialization before its single Escape. This accommodates
[slurp 1.5's startup sequence](https://github.com/emersion/slurp/blob/v1.5.0/main.c#L982-L1001),
which initializes its running flag after startup roundtrips. A protocol trace
confirmed Escape delivery and cancellation through the native selector.

Local logs, result JSON and source hashes are retained under
`build/verification/audit-fixes-2026-09-06/`. Physical screenshots remain in private
temporary directories because they can contain desktop content.

Sanitizers instrument the host harness, not the loaded compositor plugin. Input
tests use injected events; they do not certify every physical touchpad driver.
Application checks use disposable profiles. Applications discarding launch
correlation retain native placement. Other compositor, GPU and companion versions
remain outside the supported matrix. Omarchy-managed source files were untouched.
