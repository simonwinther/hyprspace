"""Cursor boundaries while following a magnified workspace or board window."""

import json
import subprocess
import time

from protocol import reply
from resize import area
from zoom import F, PAN_BUTTON, SUPER, Z, center, inspection, load_overview_fixture, primary_destinations, relative_to, released, tile, wheel


def output_box(s, name):
    return area({**next(m for m in s.data("monitors") if m["name"] == name), "reserved": [0, 0, 0, 0]})


def contains(box, point):
    return box["x"] <= point["x"] < box["x"] + box["w"] and box["y"] <= point["y"] < box["y"] + box["h"]


def tap(s, key):
    s.key(key, 1)
    s.key(key, 0)


def assert_bounded(s, box):
    point = s.data("cursorpos")
    assert contains(box, point), (box, point, s.status())
    assert s.status()["cursor_owned"]
    return point


def exercise_edges(s, box):
    start = center(box)
    for dx, dy in ((-5000, 0), (5000, 0), (0, -5000), (0, 5000),
                   (-5000, -5000), (5000, -5000), (-5000, 5000), (5000, 5000)):
        s.move(start)
        s.motion(dx, dy)
        edge = assert_bounded(s, box)
        s.motion(dx, dy)
        assert assert_bounded(s, box) == edge
        inward = (-8 if dx > 0 else 8 if dx < 0 else 0, -8 if dy > 0 else 8 if dy < 0 else 0)
        s.motion(*inward)
        moved = assert_bounded(s, box)
        assert moved == {"x": edge["x"] + inward[0], "y": edge["y"] + inward[1]}, (edge, moved, inward)
    s.move(start)
    s.motion(5000, 0)
    edge = assert_bounded(s, box)
    s.motion(0, 15)
    assert assert_bounded(s, box) == {"x": edge["x"], "y": edge["y"] + 15}


def cross(s, start, destination, absolute=False):
    s.move(start)
    if absolute:
        s.absolute_motion(destination)
    else:
        relative_to(s, destination)
    cursor = s.data("cursorpos")
    assert abs(cursor["x"] - destination[0]) <= 1 and abs(cursor["y"] - destination[1]) <= 1, (cursor, destination)


def run(s, wait_for):
    fixture = load_overview_fixture(s, wait_for)
    saved_follow = s.status()["zoom"]["cursor_follow_enabled"]
    monitors = {m["name"]: m for m in s.data("monitors")}
    original_rules = {
        name: f'{name},{m["width"]}x{m["height"]}@60,{m["x"]}x{m["y"]},{m["scale"]},transform,{m["transform"]}'
        for name, m in monitors.items()
    }
    panel = None
    locker = None
    fixture_loaded = True

    def open_workspace(workspace=11):
        s.ctl("keyword", "plugin:hyprspace:follow_mouse", "true")
        s.ctl("dispatch", "hyprspace:overview", "on")
        wait_for(lambda: len(s.status()["views"]) == 3)
        s.move(center(tile(s, workspace)))
        # Reopening can preserve the same physical position. A fresh motion
        # deliberately chooses this output before Home chooses its first tile.
        s.motion(8, 0)
        tap(s, 102)
        assert s.status()["target"]["workspace"] == workspace
        s.ctl("keyword", "plugin:hyprspace:follow_mouse", "false")

    try:
        s.ctl("keyword", "plugin:hyprspace:overview:wheel_zoom", "true")
        # Selection hover and cursor-follow are independent preferences.
        s.ctl("keyword", "plugin:hyprspace:follow_mouse", "false")
        layouts = (
            ((0, 0), (960, 0), (0, -400), 0),
            ((-960, -600), (-1440, -600), (-960, 0), 0),
            ((2200, 100), (-1000, -200), (-520, -200), 1),
        )
        for positions in layouts:
            s.close()
            for index, name in enumerate(s.names):
                m = monitors[name]
                x, y = positions[index]
                s.ctl("keyword", "monitor", f'{name},960x600@60,{x}x{y},{m["scale"]},transform,{m["transform"]}')
            wait_for(lambda: all((m["x"], m["y"]) == positions[s.names.index(m["name"])] for m in s.data("monitors")))
            s.setup("dwindle", 0, 0)
            primary_destinations(s)
            index = positions[3]
            name = s.names[index]
            workspace = 11 + index
            box = output_box(s, name)
            start = center(box)
            destinations = [center(output_box(s, other)) for other in s.names if other != name]
            open_workspace(workspace)
            s.key(Z, 1)
            inspection(s, wait_for, 1)
            s.move(center(tile(s, workspace)))
            if not s.status()["zoom"]["cursor_follow_enabled"]:
                tap(s, F)
            s.ctl("keyword", "animations:enabled", "true")
            wheel(s, -1)
            s.motion(5000, -5000)
            assert_bounded(s, box)
            s.move(start)
            wheel(s, 32)
            inspection(s, wait_for, 1)
            s.ctl("keyword", "animations:enabled", "false")
            s.move(start)
            wheel(s, -32)
            inspection(s, wait_for, 4)
            native = s.geometry()
            exercise_edges(s, box)
            for destination in destinations:
                s.move(start)
                s.absolute_motion(destination)
                assert_bounded(s, box)
            layout_boxes = [output_box(s, other) for other in s.names]
            for x in (min(b["x"] for b in layout_boxes), max(b["x"] + b["w"] for b in layout_boxes)):
                for y in (min(b["y"] for b in layout_boxes), max(b["y"] + b["h"] for b in layout_boxes)):
                    s.move(start)
                    s.absolute_motion((x, y))
                    assert_bounded(s, box)
            assert s.geometry() == native
            s.check(f"magnified follow bounds relative and absolute motion at every edge of {name}, including scaled and rotated layouts")

            for key, button in ((57, None), (None, PAN_BUTTON)):
                s.move(start)
                if key:
                    s.key(key, 1)
                else:
                    s.button(1, button)
                assert s.status()["zoom"]["pan_held"]
                s.motion(5000, -5000)
                assert_bounded(s, box)
                assert s.status()["zoom"]["pan_held"]
                if key:
                    s.key(key, 0)
                else:
                    s.button(0, button)
            s.check("Space and right-drag keep the cursor bounded while camera follow is paused")

            s.move(start)
            tap(s, F)
            for destination in destinations:
                cross(s, start, destination)
                cross(s, start, destination, absolute=True)
            s.move(start)
            tap(s, F)
            s.ctl("keyword", "animations:enabled", "true")
            wheel(s, 32)
            assert s.status()["zoom"]["extra_goal"] == 1
            cross(s, start, destinations[0])
            inspection(s, wait_for, 1)
            s.move(start)
            wheel(s, -32)
            inspection(s, wait_for, 4)
            s.key(Z, 0)
            relative_to(s, destinations[0])
            time.sleep(.12)
            cursor = s.data("cursorpos")
            assert abs(cursor["x"] - destinations[0][0]) <= 1 and abs(cursor["y"] - destinations[0][1]) <= 1
            released(s, wait_for)
            cross(s, start, destinations[0], absolute=True)
            s.check("disabling follow, requesting the fitted view and releasing Z immediately restore cross-output motion")
            s.ctl("keyword", "animations:enabled", "false")

            s.move(center(tile(s, workspace)))
            s.key(Z, 1)
            inspection(s, wait_for, 1)
            s.move(destinations[0])
            outside = s.data("cursorpos")
            tap(s, 13)  # Keyboard zoom uses the selected inspection's center.
            inspection(s, wait_for, 1.15)
            assert s.data("cursorpos") == outside
            s.motion(8, 0)
            assert not contains(box, s.data("cursorpos"))
            relative_to(s, start)
            assert_bounded(s, box)
            s.motion(5000, 0)
            assert_bounded(s, box)
            # An intentional compositor warp still takes control.
            s.move(destinations[0])
            assert not contains(box, s.data("cursorpos"))
            s.check("zoom outside an inspection does not warp the cursor; entering arms confinement and explicit warps remain available")
            s.key(Z, 0)
            s.close()

        # Restore the standard layout for foreground, board and teardown cases.
        for rule in original_rules.values():
            s.ctl("keyword", "monitor", rule)
        s.setup("dwindle", 0, 1)
        primary_destinations(s)
        name = s.names[0]
        box = output_box(s, name)
        start = center(box)
        remote = center(output_box(s, s.names[1]))
        open_workspace()
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        s.move(center(tile(s, 11)))
        wheel(s, -32)
        inspection(s, wait_for, 4)
        for namespace in ("waybar-cursor-boundary", "cursor-boundary-foreground"):
            panel = s.spawn(["python3", str(s.artifact("layer.py")), str(s.root / namespace), namespace, "pointer", "0,0"])
            layer = wait_for(lambda: s.layer(namespace))
            s.move(start)
            if namespace.startswith("waybar"):
                s.key(57, 1)
                relative_to(s, center(layer))
                assert s.status()["zoom"]["pan_held"] and s.status()["cursor_owned"]
                s.motion(-5000, 0)
                assert_bounded(s, box)
                assert s.status()["zoom"]["pan_held"]
                s.key(57, 0)
            else:
                relative_to(s, center(layer))
                wait_for(lambda: not s.status()["cursor_owned"])
                relative_to(s, remote)
                assert not contains(box, s.data("cursorpos"))
            panel.terminate()
            panel.wait(timeout=3)
            panel = None
            wait_for(lambda: not s.layer(namespace))
        s.check("Waybar retains bounded manual panning while foreground pointer interfaces release the boundary")
        s.move(start)
        s.ctl("dispatch", "hyprspace:help", "on")
        cross(s, start, remote)
        s.ctl("dispatch", "hyprspace:help", "off")
        s.key(Z, 0)
        s.close()

        s.setup("dwindle", 0, 0)
        primary_destinations(s)
        native = s.geometry()
        for workspace_drag in (False, True):
            open_workspace()
            s.key(Z, 1)
            inspection(s, wait_for, 1)
            s.move(center(tile(s, 11)))
            wheel(s, -32)
            inspection(s, wait_for, 4)
            point = start
            if not workspace_drag:
                previews = next(view["tiles"] for view in s.status()["views"] if view["monitor"] == name)
                preview = next(p for p in previews if p["window"] != "0x0" and p["x"] < box["x"] + box["w"] and
                               p["x"] + p["w"] > box["x"] and p["y"] < box["y"] + box["h"] and p["y"] + p["h"] > box["y"])
                point = ((max(preview["x"], box["x"]) + min(preview["x"] + preview["w"], box["x"] + box["w"])) / 2,
                         (max(preview["y"], box["y"]) + min(preview["y"] + preview["h"], box["y"] + box["h"])) / 2)
            s.move(point)
            s.key(SUPER, 1)
            if workspace_drag:
                s.key(56, 1)
            s.button(1)
            assert s.status()["dragging"]
            relative_to(s, remote)
            assert not contains(box, s.data("cursorpos"))
            tap(s, 1)  # Cancel without changing native window/workspace placement.
            s.button(0)
            s.key(56, 0)
            s.key(SUPER, 0)
            s.key(Z, 0)
            assert not s.status()["dragging"] and s.geometry() == native
            s.close()
        s.check("native window and workspace drags release confinement and cross outputs without changing placement on cancellation")

        s.setup("dwindle", 1, 0)
        primary_destinations(s)
        open_workspace()
        s.ctl("dispatch", "hyprspace:windowview", "flat")
        wait_for(lambda: s.status()["window_board"]["active"])
        board = s.status()["window_board"]
        assert board["monitor"] == name
        remote_address = s.windows()["hs-A"]["address"]
        for _ in range(board["count"]):
            if s.status()["window_board"]["selected_window"] == remote_address:
                break
            tap(s, 15)
        board = s.status()["window_board"]
        assert board["selected_window"] == remote_address
        selected = next(w for w in s.data("clients") if w["address"] == board["selected_window"])
        assert selected["monitor"] != monitors[name]["id"], selected
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        s.move(start)
        wheel(s, -32)
        inspection(s, wait_for, 4)
        exercise_edges(s, box)
        s.move(start)
        s.absolute_motion(remote)
        assert_bounded(s, box)
        s.move(start)
        s.key(57, 1)
        s.motion(5000, -5000)
        assert_bounded(s, box)
        assert s.status()["zoom"]["pan_held"]
        s.key(57, 0)
        s.key(Z, 0)
        relative_to(s, remote)
        released(s, wait_for)
        assert not contains(box, s.data("cursorpos"))
        s.check("window-board inspection bounds its display output even when the selected window belongs to another monitor")
        s.close()

        open_workspace()
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        s.move(center(tile(s, 11)))
        wheel(s, -32)
        inspection(s, wait_for, 4)
        s.move(start)
        s.ctl("keyword", "monitor", f"{name},disable")
        s.await_outputs(2)
        wait_for(lambda: not s.status()["zoom"]["held"])
        s.motion(30, 0)
        assert not s.status()["zoom"]["held"]
        s.key(Z, 0)
        s.close()
        s.ctl("keyword", "monitor", original_rules[name])
        s.await_outputs(3)
        s.check("removing the inspected monitor cancels confinement without retaining a stale output")

        s.setup("dwindle", 0, 1)
        primary_destinations(s)
        open_workspace()
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        s.move(center(tile(s, 11)))
        wheel(s, -32)
        inspection(s, wait_for, 4)
        locker = s.spawn([str(s.artifact("test-lock"))], stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)

        def lock_request(command):
            locker.stdin.write(command.encode() + b"\n")
            locker.stdin.flush()
            return json.loads(reply(locker))

        assert reply(locker) == "ready"
        lock_request("lock")
        wait_for(lambda: lock_request("counts")["locked"])
        assert not s.status()["zoom"]["held"]
        cross(s, start, remote)
        s.key(Z, 0)
        lock_request("unlock")
        wait_for(lambda: not lock_request("counts")["locked"])
        locker.terminate()
        locker.wait(timeout=3)
        locker = None
        s.check("session locking releases a magnified follow boundary and leaves native pointer movement available")

        open_workspace()
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        s.move(center(tile(s, 11)))
        wheel(s, -32)
        inspection(s, wait_for, 4)
        s.ctl("plugin", "unload", str(fixture))
        fixture_loaded = False
        s.ctl("plugin", "unload", str(s.plugin))
        cross(s, start, remote)
        s.key(Z, 0)
        s.ctl("plugin", "load", str(s.plugin))
        s.ctl("reload")
        fixture = load_overview_fixture(s, wait_for)
        fixture_loaded = True
        assert not s.status()["live"] and not s.status()["zoom"]["held"]
        s.check("unloading a confined overview restores native motion and reloading starts without stale confinement")
    finally:
        if locker is not None and locker.poll() is None and lock_request("counts")["locked"]:
            lock_request("unlock")
            wait_for(lambda: not lock_request("counts")["locked"])
        for process in (panel, locker):
            if process is not None and process.poll() is None:
                process.terminate()
                process.wait(timeout=3)
        for key in (F, Z, 57, 56, SUPER):
            s.key(key, 0)
        for button in (PAN_BUTTON, 272):
            s.button(0, button)
        s.close()
        for rule in original_rules.values():
            s.ctl("keyword", "monitor", rule)
        s.await_outputs(3)
        s.ctl("keyword", "animations:enabled", "false")
        if s.status()["zoom"]["cursor_follow_enabled"] != saved_follow:
            open_workspace()
            s.key(Z, 1)
            tap(s, F)
            s.key(Z, 0)
            s.close()
        s.ctl("keyword", "plugin:hyprspace:follow_mouse", "true")
        if fixture_loaded:
            s.ctl("plugin", "unload", str(fixture))
