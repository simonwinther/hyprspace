"""Prepare empty destinations without changing the native desktop or focus."""

import json
import time

from launch_regressions import delayed
from regressions import center, tile
from resize import area, inside, window_box


N, X, Z, SUPER = 49, 45, 44, 125
MIDDLE = 274


def press(s, code=N):
    s.key(code, 1)
    s.key(code, 0)


def reset(s, wait_for, source=0, destination=1):
    s.close()
    # Runtime workspace reservations and shortcut remaps must not leak into
    # subsequent cases, or into the rest of --only all.
    s.ctl("reload")
    assert not s.ctl("configerrors")
    s.setup("dwindle", source, destination)
    wait_for(lambda: not s.status()["views"])


def opened(s, wait_for):
    s.ctl("dispatch", "hyprspace:overview", "on")
    wait_for(lambda: len(s.status()["views"]) == len(s.names))
    wait_for(lambda: s.status()["keyboard_owned"])
    time.sleep(.15)


def view(s, index):
    return next(v for v in s.status()["views"] if v["monitor"] == s.names[index])


def button(s, index):
    box = view(s, index)["empty_workspace_button"]
    assert box is not None, view(s, index)
    return box


def identities(s):
    return {(w["id"], w["name"], w["monitor"]) for w in s.data("workspaces")}


def native(s):
    return {
        "window": s.data("activewindow").get("address"),
        "monitors": {
            m["name"]: (m["activeWorkspace"]["id"], m["specialWorkspace"]["id"], m["focused"])
            for m in s.data("monitors")
        },
    }


def unused(s, reserved=()):
    used = {w["id"] for w in s.data("workspaces")} | set(reserved)
    candidate = 1
    while candidate in used:
        candidate += 1
    return candidate


def selected(s, wait_for, index, expected=None):
    target = wait_for(lambda: s.status().get("target"))
    if expected is not None:
        assert target["workspace"] == expected, (target, expected)
    assert target["window"] == "0x0", target
    workspace = next(w for w in s.data("workspaces") if w["id"] == target["workspace"])
    assert workspace["monitor"] == s.names[index] and workspace["windows"] == 0, workspace
    wait_for(lambda: any(t["workspace"] == workspace["id"] for t in view(s, index)["tiles"]))
    return workspace["id"]


def allocation(s, wait_for):
    for follow in (False, True):
        reset(s, wait_for)
        s.ctl("keyword", "plugin:hyprspace:follow_mouse", str(follow).lower())
        opened(s, wait_for)
        # Establish a keyboard-selected source on output one, then use motion
        # (rather than native movecursor) to keep native focus there.
        s.move(s.preview_point("hs-A"))
        press(s, 102)  # Home
        assert s.status()["target"]["workspace"] == 11
        cursor = s.data("cursorpos")
        point = center(button(s, 1))
        s.motion(point[0] - cursor["x"], point[1] - cursor["y"])
        if not follow:
            assert s.status()["target"]["workspace"] == 11
        before = native(s)
        existing = identities(s)
        expected = unused(s)
        press(s)
        workspace = selected(s, wait_for, 1, expected)
        assert native(s) == before, (before, native(s))
        time.sleep(.2)
        assert s.status()["target"]["workspace"] == workspace
        assert identities(s) - existing == {(workspace, str(workspace), s.names[1])}
        for _ in range(3):
            press(s)
        assert selected(s, wait_for, 1) == workspace
        assert identities(s) - existing == {(workspace, str(workspace), s.names[1])}
        s.run("wtype", "-k", "Escape")
        wait_for(lambda: not s.status()["views"])
        # Closing restores ordinary pointer routing, which can refocus its
        # output. Escape must preserve every desktop's workspace identities.
        after = native(s)
        assert {name: state[:2] for name, state in after["monitors"].items()} == {
            name: state[:2] for name, state in before["monitors"].items()
        }, (before, after)
        s.check(f"follow_mouse={follow}: N allocates and reuses the pointer output's lowest free workspace without changing native focus; Escape preserves the desktop")

    reset(s, wait_for)
    # Numeric and range assignments reserve IDs even when the owning monitor
    # does not exist. The empty workspace on output three remains untouched.
    s.ctl("keyword", "workspace", "1,monitor:HS-DISCONNECTED")
    s.ctl("keyword", "workspace", f"r[2-4],monitor:{s.names[1]}")
    s.ctl("keyword", "workspace", f"5,monitor:{s.names[2]}")
    opened(s, wait_for)
    s.move(center(button(s, 0)))
    before = identities(s)
    expected = unused(s, range(1, 6))
    s.ctl("dispatch", "hyprspace:emptyworkspace")
    selected(s, wait_for, 0, expected)
    assert (13, "13", s.names[2]) in identities(s)
    assert identities(s) - before == {(expected, str(expected), s.names[0])}
    s.check("fresh allocation respects exact, range and disconnected-output reservations and does not take an empty workspace from another monitor")
    s.close()

    reset(s, wait_for)
    assert s.ctl("keyword", "workspace", "r[1-3],defaultName:hs-empty-reserved") == "ok"
    assert s.ctl("keyword", "workspace", f"name:hs-empty-reserved,monitor:{s.names[1]}") == "ok"
    assert not s.ctl("configerrors")
    opened(s, wait_for)
    s.move(center(button(s, 0)))
    expected = unused(s, range(1, 4))
    press(s)
    selected(s, wait_for, 0, expected)
    s.check("fresh allocation respects name-based monitor reservations introduced by numeric defaultName rules")
    s.close()

    reset(s, wait_for)
    assert s.ctl("keyword", "workspace", f"w[0],monitor:{s.names[1]}") == "ok"
    assert not s.ctl("configerrors")
    opened(s, wait_for)
    s.move(center(button(s, 0)))
    before = identities(s)
    response = s.run("hyprctl", "dispatch", "hyprspace:emptyworkspace")
    assert "conditional workspace monitor binding" in response, response
    assert identities(s) == before and s.status()["live"]
    assert "conditional workspace monitor binding" in view(s, 0)["empty_workspace_error"]
    s.check("unsupported conditional monitor assignments report a visible error without allocating a trial workspace")
    s.close()

    reset(s, wait_for)
    opened(s, wait_for)
    s.move(center(button(s, 0)))
    before = identities(s)
    focused = native(s)
    target = s.status()["target"]
    frame = view(s, 0)["layout"]["frames"]
    s.ctl("keyword", "plugin:hyprspace:overview:padding", "512")
    wait_for(lambda: view(s, 0)["layout"]["frames"] >= frame + 3)
    response = s.run("hyprctl", "dispatch", "hyprspace:emptyworkspace")
    assert "not enough space to show this workspace" in response, response
    wait_for(lambda: identities(s) == before)
    assert native(s) == focused and s.status()["target"] == target
    assert "not enough space" in view(s, 0)["empty_workspace_error"]
    s.ctl("keyword", "plugin:hyprspace:overview:padding", "56")
    press(s)
    selected(s, wait_for, 0)
    assert not view(s, 0)["empty_workspace_error"]
    s.check("an unrenderable prepared workspace reports an error, restores its temporary allocation and retains the prior target; normal padding recovers")
    s.close()


def reuse(s, wait_for):
    reset(s, wait_for)
    s.ctl("keyword", "workspace", f"31,monitor:{s.names[2]},persistent:true")
    wait_for(lambda: any(w["id"] == 31 for w in s.data("workspaces")))
    opened(s, wait_for)
    s.move(center(button(s, 2)))
    before = identities(s)
    press(s)
    selected(s, wait_for, 2, 13)
    assert identities(s) == before
    s.check("the output's empty active workspace takes precedence over other empty persistent destinations")
    s.close()

    reset(s, wait_for)
    for workspace in (32, 31):
        s.ctl("keyword", "workspace", f"{workspace},monitor:{s.names[0]},persistent:true")
    wait_for(lambda: {31, 32} <= {w["id"] for w in s.data("workspaces")})
    opened(s, wait_for)
    s.move(center(button(s, 0)))
    before = identities(s)
    press(s)
    selected(s, wait_for, 0, 31)
    assert identities(s) == before
    s.check("inactive empty persistent workspaces are reused in grid order")
    s.close()

    reset(s, wait_for)
    s.ctl("keyword", "workspace", f"name:hs-empty-named,monitor:{s.names[0]},persistent:true")
    workspace = wait_for(lambda: next((w for w in s.data("workspaces") if w["name"] == "hs-empty-named"), None))
    opened(s, wait_for)
    s.move(center(button(s, 0)))
    press(s)
    selected(s, wait_for, 0, workspace["id"])
    assert s.status()["target"]["name"] == "hs-empty-named"
    s.check("normal named empty workspaces remain eligible destinations")
    s.close()

    reset(s, wait_for)
    s.ctl("dispatch", "togglespecialworkspace", "hs-empty-special")
    special = next(w["id"] for w in s.data("workspaces") if w["name"] == "special:hs-empty-special")
    opened(s, wait_for)
    s.move(center(button(s, 0)))
    expected = unused(s)
    press(s)
    selected(s, wait_for, 0, expected)
    assert s.status()["target"]["workspace"] != special
    s.close()
    s.ctl("dispatch", "togglespecialworkspace", "hs-empty-special")
    s.check("empty special workspaces are excluded from allocation and reuse")


def commit_and_launch(s, wait_for):
    reset(s, wait_for)
    opened(s, wait_for)
    s.move(center(button(s, 0)))
    press(s)
    workspace = selected(s, wait_for, 0)
    # Keep the cursor on the header as the grid rearranges. Native commands
    # must commit the selected prepared destination, not its old pointer hit.
    s.run("wtype", "-k", "Return")
    wait_for(lambda: not s.status()["views"])
    assert s.data("activeworkspace")["id"] == workspace
    assert not s.data("activewindow").get("address")
    s.spawn(["python3", str(s.client), "hs-empty-enter"])
    wait_for(lambda: s.windows().get("hs-empty-enter", {}).get("workspace", {}).get("id") == workspace)
    s.check("Enter opens the prepared desktop and subsequent applications map there")

    reset(s, wait_for)
    opened(s, wait_for)
    s.move(center(button(s, 0)))
    press(s)
    workspace = selected(s, wait_for, 0)
    token = s.request("capture")
    assert token
    gate = delayed(s, wait_for, token, "hs-empty-delayed")
    s.close()
    # The destination was never activated and has no windows or persistence
    # rule, so only the captured launch's lifetime can retain this identity.
    time.sleep(.3)
    assert any(w["id"] == workspace and w["windows"] == 0 for w in s.data("workspaces"))
    gate.touch()
    wait_for(lambda: s.windows().get("hs-empty-delayed", {}).get("workspace", {}).get("id") == workspace)
    assert not s.status()["live"] and s.status()["layout_targets_unique"]
    s.check("a captured delayed launch retains its unactivated empty workspace through overview dismissal")


def buttons_and_drops(s, wait_for):
    for index in range(3):
        reset(s, wait_for, source=(index + 1) % 3, destination=index)
        opened(s, wait_for)
        for other in range(3):
            box = button(s, other)
            monitor = next(m for m in s.data("monitors") if m["name"] == s.names[other])
            work = area(monitor)
            assert inside(box, work), (box, work)
            assert box["w"] > 80 and box["h"] >= 20, box
            workspace_tiles = [t for t in view(s, other)["tiles"] if t["window"] == "0x0"]
            assert all(t["y"] >= box["y"] + box["h"] for t in workspace_tiles), (box, workspace_tiles)
        s.move(center(button(s, index)))
        before = native(s)
        s.button(1)
        s.button(0)
        workspace = selected(s, wait_for, index)
        assert native(s) == before
        s.run("grim", "-s", "1", "-o", s.names[index], str(s.root / f"empty-workspace-overview-{index}.png"))
        assert any(t["workspace"] == workspace for t in view(s, index)["tiles"])
        s.check(f"the empty-workspace button renders inside {s.names[index]}'s usable area, reserves grid space and selects without closing")

    for index in (1, 2):
        reset(s, wait_for, source=0, destination=index)
        address = s.windows()["hs-A"]["address"]
        s.ctl("dispatch", "setfloating", "address:" + address)
        s.ctl("dispatch", "resizewindowpixel", "exact 240 160,address:" + address)
        opened(s, wait_for)
        old = identities(s)
        # Hovering a button during a move must leave the grid and workspace
        # collection intact until a valid release commits the destination.
        source = s.preview_point("hs-A", .8, .8)
        destination = center(button(s, index))
        s.move(source)
        s.key(SUPER, 1)
        try:
            s.button(1)
            assert s.status()["dragging"]
            s.move(destination)
            time.sleep(.15)
            assert identities(s) == old
            assert button(s, index)
            s.button(0)
        finally:
            s.button(0)
            s.key(SUPER, 0)
        window = wait_for(lambda: s.windows()["hs-A"] if s.windows()["hs-A"]["workspace"]["id"] != 11 else None)
        monitor = next(m for m in s.data("monitors") if m["name"] == s.names[index])
        assert window["monitor"] == monitor["id"], window
        work = area(monitor)
        assert inside(window_box(window), work), (window, work)
        window_center = s.point(window, .5, .5)
        assert all(abs(a - b) <= 1 for a, b in zip(window_center, center(work))), (window, work)
        assert s.status()["live"] and not s.status()["dragging"] and s.status()["layout_targets_unique"]
        s.check(f"a floating move drop onto {s.names[index]}'s button allocates on release and places the window at the usable center")

    reset(s, wait_for)
    opened(s, wait_for)
    expected = unused(s)
    s.drag(s.preview_point("hs-A"), center(button(s, 0)), True)
    wait_for(lambda: s.windows()["hs-A"]["workspace"]["id"] == expected)
    assert not s.windows()["hs-A"]["floating"] and s.status()["layout_targets_unique"]
    assert s.status()["target"]["workspace"] == expected
    s.check("a same-output tiled move drop prepares a new workspace and retains native layout membership")

    reset(s, wait_for)
    opened(s, wait_for)
    destination = center(button(s, 1))
    s.move(s.preview_point("hs-A"))
    s.key(Z, 1)
    try:
        wait_for(lambda: s.status()["zoom"]["held"] and s.status()["zoom"]["inspection_ready"])
        assert view(s, 1)["empty_workspace_button"] is None
        source = s.preview_point("hs-A")
        before = identities(s)
        s.move(source)
        s.key(SUPER, 1)
        try:
            s.button(1)
            assert s.status()["dragging"] and not s.status()["drag"]["resize"]
            assert button(s, 1)
            s.move(destination)
            assert identities(s) == before
            s.button(0)
        finally:
            s.button(0)
            s.key(SUPER, 0)
        window = wait_for(lambda: s.windows()["hs-A"] if s.windows()["hs-A"]["workspace"]["id"] != 11 else None)
        assert window["monitor"] == next(m["id"] for m in s.data("monitors") if m["name"] == s.names[1])
        wait_for(lambda: not s.status()["zoom"]["held"] and not s.status()["zoom"]["returning"])
        assert button(s, 1) and s.status()["layout_targets_unique"]
    finally:
        s.key(Z, 0)
    s.check("a move started in zoom reveals the button, allocates only on release and returns the destination to the grid")


def cancellation(s, wait_for):
    reset(s, wait_for)
    opened(s, wait_for)
    original = identities(s)
    s.move(center(button(s, 0)))
    time.sleep(.2)
    assert identities(s) == original
    for point in (center(button(s, 1)), (2000, 50)):
        s.drag(s.preview_point("hs-A"), point, True, cancel=True)
        assert identities(s) == original
        assert s.windows()["hs-A"]["workspace"]["id"] == 11
    s.drag(s.preview_point("hs-A"), (2000, 50), True)
    assert identities(s) == original
    assert s.windows()["hs-A"]["workspace"]["id"] == 11
    s.check("button hover, cancelled button drops and releases in monitor gaps allocate nothing")

    source = s.preview_point("hs-A", .8, .8)
    destination = center(button(s, 1))
    s.move(source)
    s.key(SUPER, 1)
    try:
        s.button(1, 273)
        assert s.status()["dragging"] and s.status()["drag"]["resize"]
        s.move(destination)
        # A key press during a resize is consumed without an allocation.
        s.key(SUPER, 0)
        press(s)
        assert identities(s) == original
        s.run("wtype", "-k", "Escape")
        s.button(0, 273)
    finally:
        s.button(0, 273)
        s.key(SUPER, 0)
    assert identities(s) == original and s.windows()["hs-A"]["workspace"]["id"] == 11
    s.check("resize gestures cannot prepare or drop onto an empty workspace")


def keys(s, wait_for):
    reset(s, wait_for)
    inactive = s.run("hyprctl", "dispatch", "hyprspace:emptyworkspace")
    assert "requires an open overview" in inactive, inactive
    opened(s, wait_for)
    invalid = s.run("hyprctl", "dispatch", "hyprspace:emptyworkspace", "extra")
    assert "takes no arguments" in invalid, invalid
    s.move(center(button(s, 0)))
    s.key(N, 1)
    try:
        workspace = selected(s, wait_for, 0)
        before = identities(s)
        s.move(center(button(s, 1)))
        time.sleep(.75)
        assert identities(s) == before and s.status()["target"]["workspace"] == workspace
        # A second raw down models a repeat independently of compositor timing.
        s.key(N, 1)
        assert identities(s) == before
    finally:
        s.key(N, 0)
    press(s)
    selected(s, wait_for, 1)
    s.check("one N press owns its repeats and release while crossing outputs; a fresh press prepares the new pointer output")

    reset(s, wait_for)
    opened(s, wait_for)
    s.move(center(button(s, 0)))
    s.key(N, 1)
    try:
        selected(s, wait_for, 0)
        s.close()
        opened(s, wait_for)
        s.move(center(button(s, 1)))
        before = identities(s)
        s.key(N, 1)
        time.sleep(.15)
        assert identities(s) == before
        s.ctl("keyword", "plugin:hyprspace:overview:empty_workspace_key", "x")
        s.key(N, 1)
        assert identities(s) == before
        # The captured N release remains ours even after modifier changes.
        s.key(29, 1)
        s.key(N, 0)
        s.key(29, 0)
        press(s, X)
        selected(s, wait_for, 1)
    finally:
        s.key(29, 0)
        s.key(N, 0)
    s.check("a held workspace key keeps its repeat and release lease across close, reopen, remapping and modifier changes")

    reset(s, wait_for)
    s.ctl("keyword", "plugin:hyprspace:overview:empty_workspace_key", "x")
    opened(s, wait_for)
    s.move(center(button(s, 0)))
    before = identities(s)
    press(s)
    assert identities(s) == before
    press(s, X)
    selected(s, wait_for, 0)
    s.ctl("keyword", "plugin:hyprspace:overview:empty_workspace_key", "")
    s.move(center(button(s, 1)))
    before = identities(s)
    press(s, X)
    press(s)
    assert identities(s) == before
    # The button and dispatcher remain available with the key disabled.
    s.ctl("dispatch", "hyprspace:emptyworkspace")
    selected(s, wait_for, 1)
    s.check("the workspace key can be remapped or disabled while the dispatcher remains available")

    reset(s, wait_for)
    marker = s.root / "empty-workspace-modified"
    marker.unlink(missing_ok=True)
    s.ctl("keyword", "bind", f"CTRL,N,exec,touch {marker}")
    opened(s, wait_for)
    s.move(center(button(s, 0)))
    before = identities(s)
    s.run("wtype", "-M", "ctrl", "-k", "n", "-m", "ctrl")
    wait_for(marker.exists)
    assert identities(s) == before
    entry = s.root / "empty-workspace-entry"
    entry.unlink(missing_ok=True)
    foreground = s.spawn(["python3", str(s.artifact("layer.py")), str(entry)])
    wait_for(lambda: s.layer("hs-foreground") and not s.status()["keyboard_owned"])
    s.run("wtype", "nn")
    wait_for(lambda: entry.exists() and entry.read_text() == "nn")
    assert identities(s) == before
    s.run("wtype", "-k", "Escape")
    foreground.wait(timeout=3)
    wait_for(lambda: s.status()["keyboard_owned"])
    assert s.status()["live"]
    s.check("modified N follows native bindings and foreground typing retains N without allocating")

    reset(s, wait_for)
    s.ctl("keyword", "plugin:hyprspace:overview:zoom_key", "n")
    opened(s, wait_for)
    s.move(s.preview_point("hs-A"))
    before = identities(s)
    s.key(N, 1)
    try:
        wait_for(lambda: s.status()["zoom"]["held"])
        assert identities(s) == before
    finally:
        s.key(N, 0)
    wait_for(lambda: not s.status()["zoom"]["held"])
    s.check("a conflicting zoom shortcut retains precedence over empty-workspace allocation")


def zoom(s, wait_for):
    reset(s, wait_for)
    opened(s, wait_for)
    s.move(s.preview_point("hs-A"))
    s.key(Z, 1)
    try:
        wait_for(lambda: s.status()["zoom"]["held"] and s.status()["zoom"]["inspection_ready"])
        assert view(s, 0)["empty_workspace_button"] is None
        # N cancels inspection and presents the new destination in the grid.
        press(s)
        selected(s, wait_for, 0)
        wait_for(lambda: not s.status()["zoom"]["held"] and not s.status()["zoom"]["returning"])
        assert button(s, 0)
    finally:
        s.key(Z, 0)
    s.check("the button hides during inspection and N returns zoom to the grid before selecting the destination")


def mouse_shortcut(s, wait_for):
    for follow in (False, True):
        reset(s, wait_for)
        s.ctl("keyword", "plugin:hyprspace:follow_mouse", str(follow).lower())
        opened(s, wait_for)
        s.move(s.preview_point("hs-A"))
        press(s, 102)  # Home
        for index in range(3):
            cursor = s.data("cursorpos")
            point = center(tile(s, 11 + index))
            s.motion(point[0] - cursor["x"], point[1] - cursor["y"])
            cursor = s.data("cursorpos")
            before = native(s)
            expected = unused(s) if index != 2 else 13
            s.button(1, MIDDLE)
            s.button(0, MIDDLE)
            workspace = selected(s, wait_for, index, expected)
            assert native(s) == before and s.data("cursorpos") == cursor
            assert s.status()["live"]
            ordered = [t["workspace"] for t in view(s, index)["tiles"] if t["window"] == "0x0"]
            assert ordered == sorted(ordered), ordered
            time.sleep(.15)
            assert s.status()["target"]["workspace"] == workspace
            # A new click reuses the prepared destination instead of adding
            # another tile or disturbing its numeric position.
            existing = identities(s)
            s.button(1, MIDDLE)
            s.button(0, MIDDLE)
            assert selected(s, wait_for, index) == workspace
            assert identities(s) == existing
        s.check(f"follow_mouse={follow}: middle-click prepares the pointer monitor's empty workspace without moving the cursor, changing native focus or reordering tiles")

    reset(s, wait_for)
    opened(s, wait_for)
    s.move(s.preview_point("hs-A"))
    s.button(1, MIDDLE)
    selected(s, wait_for, 0)
    s.move(s.preview_point("hs-B"))
    existing = identities(s)
    s.button(1, MIDDLE)
    assert identities(s) == existing
    s.close()
    opened(s, wait_for)
    s.move(s.preview_point("hs-B"))
    existing = identities(s)
    s.button(1, MIDDLE)
    assert identities(s) == existing
    s.key(29, 1)
    s.button(0, MIDDLE)
    s.key(29, 0)
    s.button(1, MIDDLE)
    s.button(0, MIDDLE)
    selected(s, wait_for, 1)
    s.check("a held middle button acts once across output motion, close and reopen; its release remains consumed after modifier changes")

    reset(s, wait_for)
    opened(s, wait_for)
    s.move(s.preview_point("hs-A"))
    before = identities(s)
    for modifier in (29, SUPER):
        s.key(modifier, 1)
        s.button(1, MIDDLE)
        s.button(0, MIDDLE)
        s.key(modifier, 0)
    assert identities(s) == before and s.status()["live"]
    s.move((2000, 50))
    s.button(1, MIDDLE)
    s.button(0, MIDDLE)
    assert identities(s) == before and s.status()["live"]
    s.close()
    s.ctl("keyword", "plugin:hyprspace:overview:all_monitors", "false")
    s.move(s.point(s.windows()["hs-A"], .5, .5))
    s.ctl("dispatch", "hyprspace:overview", "on")
    wait_for(lambda: len(s.status()["views"]) == 1)
    s.move(center(area(next(m for m in s.data("monitors") if m["name"] == s.names[1]))))
    before = identities(s)
    s.button(1, MIDDLE)
    s.button(0, MIDDLE)
    assert identities(s) == before and s.status()["live"]
    s.check("modified middle-clicks, monitor gaps and outputs without an overview do not prepare a workspace")

    for drag_button in (272, 273):
        reset(s, wait_for)
        opened(s, wait_for)
        s.move(s.preview_point("hs-A"))
        s.key(SUPER, 1)
        s.button(1, drag_button)
        s.key(SUPER, 0)
        assert s.status()["dragging"]
        before = identities(s)
        s.button(1, MIDDLE)
        s.button(0, MIDDLE)
        assert identities(s) == before and s.status()["dragging"]
        s.run("wtype", "-k", "Escape")
        s.button(0, drag_button)
    s.check("middle-click cannot allocate or interrupt a provisional window move or resize")

    reset(s, wait_for)
    opened(s, wait_for)
    s.move(s.preview_point("hs-A"))
    s.key(Z, 1)
    wait_for(lambda: s.status()["zoom"]["inspection_ready"])
    s.scroll(-30, -2)
    wait_for(lambda: s.status()["zoom"]["pan_available"])
    s.button(1, 273)
    wait_for(lambda: s.status()["zoom"]["pan_held"])
    before = identities(s)
    s.button(1, MIDDLE)
    s.button(0, MIDDLE)
    assert identities(s) == before and s.status()["zoom"]["pan_held"]
    s.button(0, 273)
    s.button(1, MIDDLE)
    s.button(0, MIDDLE)
    selected(s, wait_for, 0)
    wait_for(lambda: not s.status()["zoom"]["held"] and not s.status()["zoom"]["returning"])
    s.key(Z, 0)
    s.check("middle-click is ignored during panning and returns ordinary inspection to the ordered grid before preparing a workspace")

    reset(s, wait_for)
    marker = s.root / "empty-workspace-native-middle"
    marker.unlink(missing_ok=True)
    s.ctl("keyword", "bind", f",mouse:274,exec,touch {marker}")
    s.move(s.point(s.windows()["hs-A"], .5, .5))
    s.button(1, MIDDLE)
    s.button(0, MIDDLE)
    wait_for(marker.exists)
    marker.unlink()
    opened(s, wait_for)
    entry = s.root / "empty-workspace-middle-entry"
    foreground = s.spawn(["python3", str(s.artifact("layer.py")), str(entry)])
    box = wait_for(lambda: s.layer("hs-foreground"))
    s.move((box["x"] + 70, box["y"] + 70))
    before = identities(s)
    s.button(1, MIDDLE)
    s.button(0, MIDDLE)
    wait_for(lambda: entry.with_name(entry.name + ".click").exists() or marker.exists())
    assert identities(s) == before and s.status()["live"]
    s.run("wtype", "-k", "Escape")
    foreground.wait(timeout=3)
    wait_for(lambda: s.status()["keyboard_owned"])
    s.check("middle-click retains native binding and foreground pointer routing outside overview input ownership")


def run(s, wait_for):
    try:
        allocation(s, wait_for)
        reuse(s, wait_for)
        commit_and_launch(s, wait_for)
        buttons_and_drops(s, wait_for)
        cancellation(s, wait_for)
        keys(s, wait_for)
        zoom(s, wait_for)
        mouse_shortcut(s, wait_for)
    except BaseException:
        (s.root / "empty-workspace-failure.json").write_text(json.dumps({
            "status": s.status(), "workspaces": s.data("workspaces"),
            "monitors": s.data("monitors"), "clients": s.data("clients"),
        }, indent=2))
        raise
    finally:
        s.button(0)
        s.button(0, 273)
        s.button(0, MIDDLE)
        for code in (N, X, Z, SUPER, 29):
            s.key(code, 0)
        s.close()
        s.ctl("reload")
