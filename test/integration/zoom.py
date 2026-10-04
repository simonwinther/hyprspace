"""Hold-to-zoom input, displayed geometry and transition regressions."""

import json
import math
import subprocess
import time

from protocol import reply
from resize import area, inside, window_box


Z, X, TAB, SHIFT, SUPER = 44, 45, 15, 42, 125
PAN_BUTTON = 273


def tiles(s, monitor=None):
    return {
        (view["monitor"], tile["workspace"]): tile
        for view in s.status()["views"]
        if monitor is None or view["monitor"] == monitor
        for tile in view["tiles"]
        if tile["window"] == "0x0"
    }


def tile(s, workspace, monitor=None):
    return next(value for (_, ws), value in tiles(s, monitor).items() if ws == workspace)


def center(box):
    return box["x"] + box["w"] / 2, box["y"] + box["h"] / 2


def same_geometry(before, after, tolerance=0.1):
    return before.keys() == after.keys() and all(
        abs(before[key][part] - after[key][part]) <= tolerance
        for key in before
        for part in ("x", "y", "w", "h")
    )


def released(s, wait_for):
    wait_for(lambda: not s.status()["zoom"]["held"] and not s.status()["zoom"]["returning"])


def inspection(s, wait_for, factor=None):
    def settled():
        zoom = s.status()["zoom"]
        if not zoom["inspection_ready"] or not zoom["camera_settled"] or zoom["inspection_transitioning"]:
            return None
        if factor is not None and abs(zoom["extra_factor"] - factor) > 0.0001:
            return None
        return zoom

    try:
        return wait_for(settled)
    except AssertionError:
        (s.root / "wheel-inspection-timeout.json").write_text(json.dumps({
            "expected_factor": factor, "status": s.status(), "cursor": s.data("cursorpos"),
        }, indent=2))
        raise


def wheel(s, detents):
    s.scroll(delta=15 * detents, discrete=detents)


def relative_to(s, point):
    cursor = s.data("cursorpos")
    s.motion(point[0] - cursor["x"], point[1] - cursor["y"])


def overview_cursor(s, wait_for, expected):
    return wait_for(lambda: s.status()["overview_cursor"] == expected)


def primary_destinations(s, layout="dwindle"):
    for workspace in range(21, 30):
        s.ctl("keyword", "workspace", f"{workspace},monitor:{s.names[0]},persistent:true,layout:{layout}")


def load_overview_fixture(s, wait_for):
    def animations_enabled():
        value = json.loads(s.ctl("-j", "getoption", "animations:enabled"))
        return value["int"]

    # Fixture loading schedules a deferred config reload. The private config
    # disables animations, so observe that reload before adding runtime rules.
    s.ctl("keyword", "animations:enabled", "true")
    assert animations_enabled() == 1
    fixture = s.artifact("test-overview.so")
    s.ctl("plugin", "load", str(fixture))
    wait_for(lambda: animations_enabled() == 0)
    return fixture


def opened(s, wait_for, workspace=11):
    s.ctl("dispatch", "hyprspace:overview", "on")
    wait_for(lambda: len(s.status()["views"]) == 3)
    s.move(center(tile(s, workspace)))
    # Opening tiles can overlap, so keyboard selection establishes the measured
    # destination independently of a moving pointer hit during the transition.
    s.key(102, 1)  # Home: each fixture opens on its output's first workspace.
    s.key(102, 0)
    assert s.status()["target"]["workspace"] == workspace


def zoom_edges(s, monitor):
    return next(view["zoom_edges"] for view in s.status()["views"] if view["monitor"] == monitor)


def edge_point(work, direction):
    x, y = center(work)
    return {
        "left": (work["x"] + 1, y),
        "right": (work["x"] + work["w"] - 1, y),
        "up": (x, work["y"] + 1),
        "down": (x, work["y"] + work["h"] - 1),
    }[direction]


def edge_browsing(s, wait_for):
    s.setup("dwindle", 0, 1)
    primary_destinations(s)
    s.ctl("dispatch", "movetoworkspacesilent", f'25,address:{s.windows()["hs-B"]["address"]}')
    opened(s, wait_for)
    monitor = s.names[0]
    work = area(next(m for m in s.data("monitors") if m["name"] == monitor))
    order = sorted(ws for name, ws in tiles(s) if name == monitor)
    native = s.geometry()
    active = {m["name"]: m["activeWorkspace"]["id"] for m in s.data("monitors")}

    def select(workspace):
        s.key(102, 1)
        s.key(102, 0)
        for _ in range(order.index(workspace)):
            s.key(TAB, 1)
            s.key(TAB, 0)
        assert s.status()["target"]["workspace"] == workspace

    select(25)
    s.key(Z, 1)
    wait_for(lambda: len(zoom_edges(s, monitor)) == 4)
    assert {hint["direction"]: hint["workspace"] for hint in zoom_edges(s, monitor)} == {
        "left": 24, "right": 26, "up": 21, "down": 28,
    }
    enlarged = tile(s, 25)
    s.move(center(enlarged))
    s.run("grim", "-s", "1", "-o", monitor, str(s.root / "zoom-edge-hints.png"))
    for direction, destination in (("left", 24), ("right", 26), ("up", 21), ("down", 28)):
        select(25)
        s.move(center(tile(s, 25)))
        hint = next(h for h in zoom_edges(s, monitor) if h["direction"] == direction)
        assert hint["w"] == hint["h"] == 32
        s.move(center(hint))
        wait_for(lambda: s.status()["zoom"]["workspace"] == destination)
        assert s.status()["zoom"]["monitor"] == monitor
        assert all(abs(tile(s, destination)[part] - enlarged[part]) < 0.1 for part in ("w", "h"))
        time.sleep(0.4)
        assert s.status()["zoom"]["workspace"] == destination
    assert s.geometry() == native
    assert {m["name"]: m["activeWorkspace"]["id"] for m in s.data("monitors")} == active
    s.check("all four enlarged arrow centers browse neighbors after dwell without committing or repeating")

    select(25)
    s.move(center(tile(s, 25)))
    s.move((work["x"] + work["w"] / 2, work["y"] + 47))
    wait_for(lambda: s.status()["zoom"]["workspace"] == 21)
    s.check("the top hover area reaches 48 logical pixels into the usable monitor")

    select(25)
    s.move(center(tile(s, 25)))
    s.move(edge_point(work, "right"))
    wait_for(lambda: s.status()["zoom"]["workspace"] == 26)
    s.move(center(tile(s, 26)))
    s.move(edge_point(work, "right"))
    wait_for(lambda: s.status()["zoom"]["workspace"] == 27)
    assert not any(h["direction"] == "right" for h in zoom_edges(s, monitor))
    time.sleep(0.4)
    assert s.status()["zoom"]["workspace"] == 27
    select(25)
    s.move(center(tile(s, 25)))
    s.move((work["x"] + work["w"] - 1, work["y"] + 1))
    time.sleep(0.4)
    assert s.status()["zoom"]["workspace"] == 25
    s.move(center(tile(s, 25)))
    s.move(edge_point(work, "right"))
    s.move(center(tile(s, 25)))
    time.sleep(0.4)
    assert s.status()["zoom"]["workspace"] == 25
    s.check("zoom edge exit and reentry permit one more step; corners and early exits do not browse")

    s.ctl("keyword", "plugin:hyprspace:follow_mouse", "false")
    assert not zoom_edges(s, monitor)
    assert same_geometry({(monitor, 25): enlarged}, {(monitor, 25): tile(s, 25)})
    s.move(edge_point(work, "right"))
    time.sleep(0.4)
    assert s.status()["zoom"]["workspace"] == 25
    s.ctl("keyword", "plugin:hyprspace:follow_mouse", "true")
    time.sleep(0.4)
    assert s.status()["zoom"]["workspace"] == 25
    s.move((work["x"] + work["w"] - 2, work["y"] + work["h"] / 2))
    wait_for(lambda: s.status()["zoom"]["workspace"] == 26)
    s.move(center(tile(s, 26)))
    s.move(edge_point(work, "right"))
    select(11)
    time.sleep(0.4)
    assert s.status()["zoom"]["workspace"] == 11
    s.check("follow_mouse recovery requires real motion; keyboard browsing cancels pending dwell")

    panel_namespace = "waybar-workspace-slider-zoom-test"
    panel = s.spawn(["python3", str(s.artifact("layer.py")), str(s.root / "zoom-panel"), panel_namespace, "bottom"])
    try:
        wait_for(lambda: s.layer(panel_namespace))
        wait_for(lambda: area(next(m for m in s.data("monitors") if m["name"] == monitor))["y"] > work["y"])
        panel_work = area(next(m for m in s.data("monitors") if m["name"] == monitor))
        select(25)
        wait_for(lambda: any(h["direction"] == "up" and h["y"] >= panel_work["y"] for h in zoom_edges(s, monitor)))
        s.move(center(tile(s, 25)))
        up = next(h for h in zoom_edges(s, monitor) if h["direction"] == "up")
        assert up["y"] >= panel_work["y"] and panel_work["y"] > work["y"]
        panel_native = s.geometry()
        s.move(center(up))
        assert any(h["direction"] == "up" and h["pending"] for h in zoom_edges(s, monitor))
        box = s.layer(panel_namespace)
        s.move((box["x"] + 20, box["y"] + 20))
        wait_for(lambda: not s.status()["cursor_owned"])
        time.sleep(0.4)
        assert s.status()["zoom"]["held"] and s.status()["zoom"]["workspace"] == 25
        # Returning to exactly the previous arrow point is still real motion:
        # pointer coordinates must have followed the intervening panel visit.
        s.move(center(up))
        try:
            wait_for(lambda: s.status()["zoom"]["workspace"] == up["workspace"])
        except AssertionError:
            (s.root / "zoom-top-panel-failure.json").write_text(json.dumps({
                "status": s.status(), "cursor": s.data("cursorpos"),
                "monitors": s.data("monitors"), "layers": s.data("layers"),
                "up": up, "panel_work": panel_work,
            }, indent=2))
            raise
        assert s.status()["cursor_owned"] and s.geometry() == panel_native
        assert {m["name"]: m["activeWorkspace"]["id"] for m in s.data("monitors")} == active
        s.run("grim", "-s", "1", "-o", monitor, str(s.root / "zoom-top-panel.png"))
        s.check("top arrow recovers after a panel handoff, including return to the exact same point")
    finally:
        panel.terminate()
        panel.wait(timeout=5)
        wait_for(lambda: not s.layer(panel_namespace))
        wait_for(lambda: area(next(m for m in s.data("monitors") if m["name"] == monitor)) == work)

    select(25)
    s.move(center(tile(s, 25)))
    s.move(edge_point(work, "right"))
    s.key(Z, 0)
    released(s, wait_for)
    time.sleep(0.4)
    assert s.status()["target"]["workspace"] == 25
    s.key(Z, 1)
    wait_for(lambda: len(zoom_edges(s, monitor)) == 4)
    time.sleep(0.4)
    assert s.status()["zoom"]["workspace"] == 25
    s.move(center(tile(s, 25)))
    s.move(edge_point(work, "right"))
    s.ctl("keyword", "workspace", "26,persistent:false")
    wait_for(lambda: (monitor, 26) not in tiles(s))
    time.sleep(0.4)
    assert s.status()["zoom"]["workspace"] == 25
    s.ctl("keyword", "workspace", f"26,monitor:{monitor},persistent:true,layout:dwindle")
    wait_for(lambda: (monitor, 26) in tiles(s))
    s.check("zoom release and disappearing neighbors cancel dwell and stationary re-presses do not arm it")

    s.move(center(tile(s, 25)))
    s.move(edge_point(work, "right"))
    s.ctl("keyword", "monitor", f"{monitor},addreserved,36,14,12,18")
    time.sleep(0.4)
    assert s.status()["zoom"]["workspace"] == 25
    s.ctl("keyword", "monitor", f"{monitor},addreserved,0,0,0,0")
    wait_for(lambda: same_geometry({(monitor, 25): enlarged}, {(monitor, 25): tile(s, 25)}))
    s.check("changed usable monitor bounds invalidate a pending zoom edge")

    s.ctl("keyword", "plugin:hyprspace:overview:padding", "0")
    wait_for(lambda: any(h["direction"] == "down" for h in zoom_edges(s, monitor)))
    s.run("grim", "-s", "1", "-o", monitor, str(s.root / "zoom-edge-labels-no-padding.png"))
    s.ctl("keyword", "plugin:hyprspace:overview:padding", "56")
    s.key(Z, 0)
    released(s, wait_for)
    s.close()

    s.ctl("keyword", "animations:enabled", "true")
    try:
        opened(s, wait_for)
        select(25)
        s.key(Z, 1)
        s.move(edge_point(work, "right"))
        wait_for(lambda: len(zoom_edges(s, monitor)) == 4)
        assert s.status()["zoom"]["workspace"] == 25
        wait_for(lambda: s.status()["zoom"]["workspace"] == 26)
        wait_for(lambda: zoom_edges(s, monitor))
        time.sleep(0.4)
        assert s.status()["zoom"]["workspace"] == 26
        s.move(center(tile(s, 26)))
        s.run("grim", "-s", "1", "-o", monitor, str(s.root / "zoom-edge-browsed.png"))
        s.move(edge_point(work, "right"))
        s.close()
        s.key(Z, 0)
        time.sleep(0.4)
        assert not s.status()["views"]
        s.check("intentional edge entry waits for animation, browses once while stationary, and closing cancels dwell")
    finally:
        s.key(Z, 0)
        s.ctl("keyword", "animations:enabled", "false")
        s.close()


def lifecycle(s, wait_for):
    defaults = ("zoom_key", "padding", "workspace_labels", "include_special", "wheel_zoom")
    saved = {
        key: json.loads(s.ctl("-j", "getoption", "plugin:hyprspace:overview:" + key))
        for key in defaults
    }
    secondary = None
    layer = None
    try:
        s.ctl("keyword", "animations:enabled", "false")
        s.ctl("keyword", "plugin:hyprspace:follow_mouse", "true")
        s.ctl("keyword", "plugin:hyprspace:overview:zoom_key", "z")
        s.ctl("keyword", "plugin:hyprspace:overview:wheel_zoom", "true")
        s.ctl("keyword", "plugin:hyprspace:overview:include_special", "false")
        s.setup("dwindle", 0, 1)
        for workspace in range(21, 30):
            s.ctl("keyword", "workspace", f"{workspace},monitor:{s.names[0]},persistent:true,layout:dwindle")
        opened(s, wait_for)
        baseline = tiles(s)
        assert len(tiles(s, s.names[0])) == 10
        native = s.geometry()
        work = area(next(m for m in s.data("monitors") if m["name"] == s.names[0]))
        s.run("grim", "-s", "1", "-o", s.names[0], str(s.root / "baseline-grid.png"))

        s.key(Z, 1)
        wait_for(lambda: s.status()["zoom"]["held"])
        wait_for(lambda: tile(s, 11)["w"] > baseline[(s.names[0], 11)]["w"] * 2)
        enlarged = tile(s, 11)
        s.run("grim", "-s", "1", "-o", s.names[0], str(s.root / "held-zoom.png"))
        assert inside(enlarged, work)
        assert enlarged["w"] >= work["w"] * 0.75
        assert abs(enlarged["w"] / enlarged["h"] - work["w"] / work["h"]) < 0.001
        assert same_geometry(
            {key: value for key, value in baseline.items() if key[0] != s.names[0]},
            {key: value for key, value in tiles(s).items() if key[0] != s.names[0]},
        )
        counts = next(view["layout"] for view in s.status()["views"] if view["monitor"] == s.names[0])
        measured = wait_for(lambda: (value if (value := next(view["layout"] for view in s.status()["views"] if view["monitor"] == s.names[0]))["frames"] >= counts["frames"] + 12 else None))
        assert all(measured[key] == counts[key] for key in ("grid_layouts", "window_layouts", "spread_layouts")), (counts, measured)
        s.check("steady held zoom renders twelve frames without additional grid or window layout solves")
        s.move(s.preview_point("hs-B"))
        assert s.status()["target"]["workspace"] == 11
        s.key(TAB, 1)
        s.key(TAB, 0)
        wait_for(lambda: s.status()["zoom"]["workspace"] == 21)
        assert s.status()["zoom"]["monitor"] == s.names[0]
        assert abs(tile(s, 21)["w"] - enlarged["w"]) < 0.1
        s.key(SHIFT, 1)
        s.key(TAB, 1)
        s.key(TAB, 0)
        s.key(SHIFT, 0)
        assert s.status()["zoom"]["workspace"] == 11
        s.key(106, 1)  # Right
        s.key(106, 0)
        assert s.status()["zoom"]["workspace"] == 21
        s.move(center(tile(s, 21)))
        s.scroll()
        assert s.status()["zoom"]["workspace"] == 21
        assert s.status()["target"]["workspace"] == 21
        # The release belongs to its original press despite changed modifiers.
        s.key(SHIFT, 1)
        s.key(Z, 0)
        s.key(SHIFT, 0)
        released(s, wait_for)
        wait_for(lambda: same_geometry(baseline, tiles(s)))
        assert s.status()["target"]["workspace"] == 21
        assert s.geometry() == native
        s.move(s.preview_point("hs-B"))
        assert s.status()["target"]["workspace"] == 12
        s.check("hold z fits one of ten workspaces; keyboard browsing stays on its output and pointer selection resumes after release")

        # Already fitting one workspace must not shrink or displace it.
        single = tile(s, 12)
        s.key(Z, 1)
        wait_for(lambda: s.status()["zoom"]["held"])
        assert all(abs(tile(s, 12)[part] - single[part]) < 0.1 for part in ("x", "y", "w", "h"))
        inspection(s, wait_for, 1)
        s.move(center(single))
        wheel(s, -2)
        inspection(s, wait_for, 1.15 ** 2)
        assert abs(tile(s, 12)["w"] / single["w"] - 1.15 ** 2) < 0.0001
        wheel(s, 32)
        inspection(s, wait_for, 1)
        assert same_geometry({(s.names[1], 12): single}, {(s.names[1], 12): tile(s, 12)})
        s.key(Z, 0)
        released(s, wait_for)
        s.check("one-workspace outputs retain their held fit, accept wheel magnification and restore that exact baseline")

        s.move(center(tile(s, 11)))
        for modifier in (SHIFT, 29, SUPER, 56):
            s.key(modifier, 1)
            s.key(Z, 1)
            assert not s.status()["zoom"]["held"]
            s.key(Z, 0)
            s.key(modifier, 0)
        assert same_geometry(baseline, tiles(s))
        # Duplicate press notifications must not require two releases.
        s.key(Z, 1)
        s.key(Z, 1)
        s.key(Z, 0)
        released(s, wait_for)
        s.check("modified zoom keys keep native routing and repeated presses cannot strand zoom")

        s.close()
        s.ctl("keyword", "plugin:hyprspace:overview:zoom_key", "X")
        opened(s, wait_for)
        s.key(Z, 1)
        assert not s.status()["zoom"]["held"]
        s.key(Z, 0)
        s.key(X, 1)
        wait_for(lambda: s.status()["zoom"]["held"])
        s.key(X, 0)
        released(s, wait_for)
        s.close()
        s.ctl("keyword", "plugin:hyprspace:overview:zoom_key", "")
        opened(s, wait_for)
        for code in (Z, X):
            s.key(code, 1)
            assert not s.status()["zoom"]["held"]
            s.key(code, 0)
        s.close()
        s.ctl("keyword", "plugin:hyprspace:overview:zoom_key", "z")
        s.check("zoom key can be changed to another XKB key or disabled with an empty value")

        opened(s, wait_for)
        s.key(Z, 1)
        s.close()
        opened(s, wait_for)
        assert not s.status()["zoom"]["held"]
        s.key(Z, 1)  # An old repeat must not rearm the reopened overview.
        assert not s.status()["zoom"]["held"]
        s.key(Z, 0)
        assert not s.status()["zoom"]["held"]
        s.key(Z, 1)
        wait_for(lambda: s.status()["zoom"]["held"])
        s.key(Z, 0)
        released(s, wait_for)
        s.check("closing and reopening while z is held requires a new press and consumes the old release")

        secondary = s.spawn([str(s.artifact("test-pointer"))], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        assert reply(secondary) == "ready"

        def other_key(code, state):
            secondary.stdin.write(f"key {code} {state}\n")
            secondary.stdin.flush()
            assert reply(secondary) == "ok"

        s.key(Z, 1)
        other_key(Z, 1)
        inspection(s, wait_for, 1)
        s.move(center(tile(s, 11)))
        wheel(s, -2)
        inspection(s, wait_for, 1.15 ** 2)
        s.key(Z, 0)
        assert s.status()["zoom"]["held"]
        assert abs(s.status()["zoom"]["extra_factor"] - 1.15 ** 2) < 0.0001
        other_key(Z, 0)
        released(s, wait_for)
        other_key(Z, 1)
        inspection(s, wait_for, 1)
        wheel(s, -2)
        inspection(s, wait_for, 1.15 ** 2)
        secondary.terminate()
        secondary.wait(timeout=3)
        secondary = None
        released(s, wait_for)
        s.check("zoom captures are per keyboard and device removal clears a held zoom")

        s.key(Z, 1)
        inspection(s, wait_for, 1)
        s.move(center(tile(s, 11)))
        wheel(s, -2)
        inspection(s, wait_for, 1.15 ** 2)
        s.button(1, PAN_BUTTON)
        wait_for(lambda: s.status()["zoom"]["panning"])
        s.move(edge_point(work, "right"))
        entry = s.root / "zoom-foreground"
        layer = s.spawn(["python3", str(s.artifact("layer.py")), str(entry)])
        wait_for(lambda: s.layer("hs-foreground"))
        wait_for(lambda: not s.status()["keyboard_owned"])
        released(s, wait_for)
        assert not s.status()["zoom"]["pan_held"] and not s.status()["zoom"]["panning"]
        s.button(0, PAN_BUTTON)
        s.key(Z, 0)
        layer.terminate()
        layer.wait(timeout=3)
        layer = None
        wait_for(lambda: s.status()["keyboard_owned"])
        assert not s.status()["zoom"]["held"]
        s.key(Z, 1)
        wait_for(lambda: s.status()["zoom"]["held"])
        s.key(Z, 0)
        released(s, wait_for)
        s.check("foreground keyboard handoff cancels zoom and its right-button grip; restoration requires a fresh press")
        s.close()

        wheel_inspection(s, wait_for)
        edge_browsing(s, wait_for)
        reload_and_removal(s, wait_for)
        gestures(s, wait_for)
        transitions(s, wait_for)
        wheel_transitions(s, wait_for)
        camera_pan(s, wait_for)
        pan_transitions(s, wait_for)
        pan_input_lifecycle(s, wait_for)
        scrolling(s, wait_for)
        transformed(s, wait_for)
        wheel_teardown(s, wait_for)
    finally:
        for process in (secondary, layer):
            if process is not None and process.poll() is None:
                process.terminate()
                process.wait(timeout=3)
        for code in (Z, X, SHIFT, SUPER, 29, 56):
            s.key(code, 0)
        for button in (272, 273):
            s.button(0, button)
        s.ctl("keyword", "animations:enabled", "false")
        s.close()
        for workspace in range(21, 30):
            s.ctl("keyword", "workspace", f"{workspace},persistent:false")
        for key, value in saved.items():
            setting = value["str"] if "str" in value else str(value["int"])
            s.ctl("keyword", "plugin:hyprspace:overview:" + key, setting)
        s.ctl("keyword", "plugin:hyprspace:follow_mouse", "true")


def reload_and_removal(s, wait_for):
    opened(s, wait_for)
    s.key(Z, 1)
    wait_for(lambda: s.status()["zoom"]["held"])
    inspection(s, wait_for, 1)
    s.move(center(tile(s, 11)))
    wheel(s, -3)
    inspection(s, wait_for, 1.15 ** 3)
    s.ctl("reload")
    released(s, wait_for)
    assert s.status()["zoom"]["extra_factor"] == s.status()["zoom"]["extra_goal"] == 1
    assert s.status()["live"] and s.ctl("configerrors") == ""
    s.key(Z, 1)  # Repeat from the press preceding the actual config reload.
    assert not s.status()["zoom"]["held"]
    s.key(Z, 0)
    s.key(Z, 1)
    wait_for(lambda: s.status()["zoom"]["held"])
    s.key(Z, 0)
    released(s, wait_for)
    s.check("a real config reload cancels held zoom and requires a new physical press")
    s.close()

    # Reloading drops runtime workspace rules; recreate the measured grid.
    for workspace in range(21, 30):
        s.ctl("keyword", "workspace", f"{workspace},monitor:{s.names[0]},persistent:true,layout:dwindle")
    opened(s, wait_for)
    s.key(Z, 1)
    s.key(TAB, 1)
    s.key(TAB, 0)
    wait_for(lambda: s.status()["zoom"]["workspace"] == 21)
    inspection(s, wait_for, 1)
    s.move(center(tile(s, 21)))
    wheel(s, -3)
    inspection(s, wait_for, 1.15 ** 3)
    assert not any(m["activeWorkspace"]["id"] == 21 for m in s.data("monitors"))
    s.ctl("keyword", "workspace", "21,persistent:false")
    wait_for(lambda: (s.names[0], 21) not in tiles(s))
    released(s, wait_for)
    assert s.status()["live"]
    assert s.status()["target"]["workspace"] in {workspace for monitor, workspace in tiles(s) if monitor == s.names[0]}
    s.key(Z, 0)
    s.check("removing the enlarged inactive persistent workspace cancels zoom and retains a valid command destination")
    s.close()
    s.ctl("keyword", "workspace", f"21,monitor:{s.names[0]},persistent:true,layout:dwindle")


def wheel_inspection(s, wait_for):
    monitor = s.names[0]
    fixture = load_overview_fixture(s, wait_for)
    try:
        s.setup("dwindle", 0, 1)
        work = area(next(m for m in s.data("monitors") if m["name"] == monitor))
        primary_destinations(s)
        opened(s, wait_for)
        assert len(tiles(s, monitor)) == 10
        grid = tiles(s)
        native = s.geometry()
        active = {m["name"]: m["activeWorkspace"]["id"] for m in s.data("monitors")}

        def axis(delta, value120, direction=0, source=0, orientation=0):
            s.ctl("dispatch", "hyprspace-test:axis", f"{source} {orientation} {delta} {value120} {direction}")

        for invalid in ("", "4 0 15 120 0", "0 2 15 120 0", "0 0 nan 120 0",
                        "0 0 inf 120 0", "0 0 1e100 0 0", "0 0 -268435456 0 0", "0 0 15 -2147483648 0",
                        "0 0 15 2147483648 0", "0 0 15 120 2", "0 0 15 120 0 trailing"):
            response = s.run("hyprctl", "dispatch", "hyprspace-test:axis", invalid)
            assert "expected source" in response, response
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        fitted = tile(s, 11)
        s.move((fitted["x"] + fitted["w"] * 0.31, fitted["y"] + fitted["h"] * 0.42))
        pointer = s.data("cursorpos")
        anchor = ((pointer["x"] - fitted["x"]) / fitted["w"], (pointer["y"] - fitted["y"]) / fitted["h"])
        counts = {v["monitor"]: v["layout"] for v in s.status()["views"]}
        wait_for(lambda: all(v["layout"]["frames"] >= counts[v["monitor"]]["frames"] + 3 for v in s.status()["views"]))
        captures = s.status()["resources"]["captures"]
        wheel(s, -1)
        inspection(s, wait_for, 1.15)
        enlarged = tile(s, 11)
        assert abs(enlarged["w"] / fitted["w"] - 1.15) < 0.0001
        assert abs(enlarged["x"] + anchor[0] * enlarged["w"] - pointer["x"]) < 0.1
        assert abs(enlarged["y"] + anchor[1] * enlarged["h"] - pointer["y"]) < 0.1
        assert inside(fitted, enlarged, 0.1)
        assert same_geometry({k: v for k, v in grid.items() if k[0] != monitor},
                             {k: v for k, v in tiles(s).items() if k[0] != monitor})
        s.move((work["x"] + work["w"] * 0.63, work["y"] + work["h"] * 0.58))
        assert same_geometry({(monitor, 11): enlarged}, {(monitor, 11): tile(s, 11)})
        s.move(s.preview_point("hs-A", 0.55, 0.55))
        selected = s.status()["target"]
        expected = s.point(s.windows()["hs-A"], 0.55, 0.55)
        assert selected["window"] == s.windows()["hs-A"]["address"]
        assert abs(selected["x"] - expected[0]) < 2.5 and abs(selected["y"] - expected[1]) < 2.5
        assert abs(s.status()["zoom"]["extra_factor"] - 1.15) < 0.0001
        assert all(s.status()["resources"]["captures"][key] == captures[key] for key in ("attempts", "bytes"))
        s.check("wheel zoom preserves an off-center pointer anchor, native hit mapping and other outputs; motion does not pan")

        wheel(s, 32)
        inspection(s, wait_for, 1)
        assert same_geometry({(monitor, 11): fitted}, {(monitor, 11): tile(s, 11)})
        s.move(center(fitted))
        for direction in (0, 1):
            axis(-3.75, -30, direction)
        inspection(s, wait_for, math.sqrt(1.15))
        wheel(s, 32)
        inspection(s, wait_for, 1)
        s.ctl("keyword", "input:scroll_factor", "0.5")
        try:
            axis(-15, -120)
            inspection(s, wait_for, math.sqrt(1.15))
        finally:
            s.ctl("keyword", "input:scroll_factor", "1")
        wheel(s, 32)
        inspection(s, wait_for, 1)
        axis(-3.75, 0)
        inspection(s, wait_for, 1.15 ** 0.25)
        s.check("native fractional value120, delta fallback, relative direction and one device-factor application retain wheel precision")

        s.ctl("keyword", "bind", ",mouse_up,workspace,22")
        s.ctl("keyword", "bind", ",mouse_down,workspace,22")
        try:
            wheel(s, -32)
            inspection(s, wait_for, 4)
            limit = tile(s, 11)
            wheel(s, -32)
            assert same_geometry({(monitor, 11): limit}, {(monitor, 11): tile(s, 11)})
            wheel(s, 1)
            inspection(s, wait_for, 4 / 1.15)
            s.move(center(tile(s, 12)))
            wheel(s, -3)
            assert abs(s.status()["zoom"]["extra_goal"] - 4 / 1.15) < 0.0001
            s.move(center(work))
            wheel(s, 32)
            inspection(s, wait_for, 1)
            wheel(s, 32)
            assert same_geometry({(monitor, 11): fitted}, {(monitor, 11): tile(s, 11)})
            wheel(s, -1)
            inspection(s, wait_for, 1.15)
        finally:
            s.ctl("keyword", "unbind", ",mouse_up")
            s.ctl("keyword", "unbind", ",mouse_down")
        assert s.geometry() == native
        assert {m["name"]: m["activeWorkspace"]["id"] for m in s.data("monitors")} == active
        s.check("magnification stays within 1–4× and reverses immediately at limits; native wheel binds remain blocked on another output and at limits")

        # Restore the measured fixture before testing navigation and release.
        primary_destinations(s)
        wait_for(lambda: len(tiles(s, monitor)) == 10 and (monitor, 21) in tiles(s))
        s.move(center(tile(s, 11)))
        wheel(s, 32)
        inspection(s, wait_for, 1)
        s.move(center(fitted))
        wheel(s, -1)
        inspection(s, wait_for, 1.15)
        s.move(center(fitted))
        wheel(s, -32)
        inspection(s, wait_for, 4)
        edge = edge_point(work, "right")
        box = tile(s, 11)
        assert box["x"] <= edge[0] < box["x"] + box["w"] and box["y"] <= edge[1] < box["y"] + box["h"]
        s.move(edge)
        time.sleep(0.4)
        assert s.status()["zoom"]["workspace"] == 11 and not zoom_edges(s, monitor)
        wheel(s, 32)
        inspection(s, wait_for, 1)
        time.sleep(0.4)
        assert s.status()["zoom"]["workspace"] == 11
        s.move(center(fitted))
        wheel(s, -3)
        inspection(s, wait_for, 1.15 ** 3)
        s.key(TAB, 1)
        s.key(TAB, 0)
        inspection(s, wait_for, 1)
        assert s.status()["zoom"]["workspace"] == 21
        assert abs(tile(s, 21)["w"] - fitted["w"]) < 0.1
        s.key(102, 1)
        s.key(102, 0)
        inspection(s, wait_for, 1)
        s.move(center(tile(s, 11)))
        wheel(s, -2)
        inspection(s, wait_for, 1.15 ** 2)
        s.ctl("keyword", "plugin:hyprspace:overview:wheel_zoom", "false")
        wait_for(lambda: s.status()["zoom"]["extra_factor"] == s.status()["zoom"]["extra_goal"] == 1 and not s.status()["zoom"]["inspection_transitioning"])
        assert not s.status()["zoom"]["inspection_ready"]
        wheel(s, -3)
        assert s.status()["zoom"]["extra_factor"] == 1
        s.ctl("keyword", "plugin:hyprspace:overview:wheel_zoom", "true")
        inspection(s, wait_for, 1)
        s.check("inspection pauses edge browsing, keyboard workspace changes refit and disabling wheel_zoom restores the held baseline")

        s.move(center(tile(s, 11)))
        wheel(s, -2)
        inspection(s, wait_for, 1.15 ** 2)
        s.ctl("keyword", "monitor", f"{monitor},addreserved,36,14,12,18")
        inspection(s, wait_for, 1)
        changed = tile(s, 11)
        assert changed["w"] < fitted["w"] or changed["h"] < fitted["h"]
        wheel(s, -2)
        inspection(s, wait_for, 1.15 ** 2)
        s.ctl("keyword", "monitor", f"{monitor},addreserved,0,0,0,0")
        inspection(s, wait_for, 1)
        assert same_geometry({(monitor, 11): fitted}, {(monitor, 11): tile(s, 11)})
        axis(-1e8, 0)
        inspection(s, wait_for, 4)
        assert all(math.isfinite(tile(s, 11)[part]) for part in ("x", "y", "w", "h"))
        wheel(s, 32)
        inspection(s, wait_for, 1)
        s.check("usable-bound changes clear and refit magnification; extreme finite input remains bounded with finite geometry")

        namespace = "waybar-wheel-inspection-test"
        panel = s.spawn(["python3", str(s.artifact("layer.py")), str(s.root / "wheel-panel"), namespace, "bottom"])
        try:
            wait_for(lambda: s.layer(namespace))
            wait_for(lambda: area(next(m for m in s.data("monitors") if m["name"] == monitor))["y"] > work["y"])
            inspection(s, wait_for, 1)
            s.move(center(tile(s, 11)))
            wheel(s, -2)
            inspection(s, wait_for, 1.15 ** 2)
            panel_box = s.layer(namespace)
            s.move((panel_box["x"] + 20, panel_box["y"] + 20))
            wait_for(lambda: not s.status()["cursor_owned"])
            wheel(s, -3)
            assert s.status()["keyboard_owned"] and abs(s.status()["zoom"]["extra_factor"] - 1.15 ** 2) < 0.0001
            s.move(center(tile(s, 11)))
            wait_for(lambda: s.status()["cursor_owned"])
            assert abs(s.status()["zoom"]["extra_factor"] - 1.15 ** 2) < 0.0001
            s.check("pointer-only panel handoff receives wheel input without changing inspection; return retains magnification")
        finally:
            panel.terminate()
            panel.wait(timeout=5)
            wait_for(lambda: not s.layer(namespace))
            wait_for(lambda: area(next(m for m in s.data("monitors") if m["name"] == monitor)) == work)
            inspection(s, wait_for, 1)
            s.move(center(tile(s, 11)))

        primary_destinations(s)
        wait_for(lambda: len(tiles(s, monitor)) == 10)
        s.move(center(tile(s, 11)))
        wheel(s, 32)
        inspection(s, wait_for, 1)
        s.move(center(fitted))
        warm = {v["monitor"]: v["layout"]["frames"] for v in s.status()["views"]}
        wait_for(lambda: all(v["layout"]["frames"] >= warm[v["monitor"]] + 3 for v in s.status()["views"]))
        counts = {v["monitor"]: v["layout"] for v in s.status()["views"]}
        captures = s.status()["resources"]["captures"]
        wheel(s, -2)
        inspection(s, wait_for, 1.15 ** 2)
        frame = next(v["layout"]["frames"] for v in s.status()["views"] if v["monitor"] == monitor)
        wait_for(lambda: next(v["layout"]["frames"] for v in s.status()["views"] if v["monitor"] == monitor) >= frame + 12)
        measured = s.status()
        for view in measured["views"]:
            before = counts[view["monitor"]]
            assert all(view["layout"][key] == before[key] for key in ("grid_layouts", "window_layouts", "spread_layouts"))
        assert all(measured["resources"]["captures"][key] == captures[key] for key in ("attempts", "bytes")), (captures, measured["resources"])
        assert measured["resources"]["textures"]["bytes"] <= measured["resources"]["textures"]["limit_bytes"]
        assert measured["resources"]["textures"]["entries"] <= measured["resources"]["textures"]["limit_entries"]
        s.key(Z, 0)
        released(s, wait_for)
        assert same_geometry(grid, tiles(s))
        s.close()
        wait_for(lambda: s.status()["resources"]["captures"]["bytes"] == s.status()["resources"]["textures"]["bytes"] == 0)
        s.check("wheel magnification reuses captures and layout solves across twelve settled frames, and close releases resources")
    finally:
        s.key(Z, 0)
        s.close()
        s.ctl("keyword", "plugin:hyprspace:overview:wheel_zoom", "true")
        s.ctl("keyword", "input:scroll_factor", "1")
        s.ctl("keyword", "monitor", f"{monitor},addreserved,0,0,0,0")
        s.ctl("plugin", "unload", str(fixture))


def camera_pan(s, wait_for):
    monitor = s.names[0]
    fixture = load_overview_fixture(s, wait_for)
    saved_cursor = None

    def overrides(arguments="probe"):
        return json.loads(s.run("hyprctl", "dispatch", "hyprspace-test:cursor-overrides", arguments))

    def restore_override(group, name):
        overrides(f"set {group} {name}" if name else f"clear {group}")

    try:
        s.setup("dwindle", 0, 1)
        primary_destinations(s)
        work = area(next(m for m in s.data("monitors") if m["name"] == monitor))
        saved_cursor = overrides()
        overrides("set 0 crosshair")
        overrides("set 1 ew-resize")
        opened(s, wait_for)
        grid = tiles(s)
        native = s.geometry()
        active = {m["name"]: m["activeWorkspace"]["id"] for m in s.data("monitors")}
        overview_cursor(s, wait_for, "default")
        assert overrides()["window_edge"] == ""
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        fitted = tile(s, 11)
        s.move(center(fitted))
        s.button(1, PAN_BUTTON)
        state = s.status()
        assert state["zoom"]["pan_held"] and not state["zoom"]["panning"] and not state["dragging"]
        s.motion(14, 11)
        wheel(s, -3)
        assert same_geometry({(monitor, 11): fitted}, {(monitor, 11): tile(s, 11)})
        assert s.status()["zoom"]["extra_goal"] == 1 and s.status()["live"]
        overview_cursor(s, wait_for, "default")
        s.button(0, PAN_BUTTON)
        assert not s.status()["zoom"]["pan_held"] and s.status()["zoom"]["held"]
        s.check("right-button grip at the minimum fit keeps the view stable and consumes wheel input without dismissal")

        s.move(center(fitted))
        wheel(s, -2)
        inspection(s, wait_for, 1.15 ** 2)
        overview_cursor(s, wait_for, "grab")
        assert s.status()["zoom"]["pan_available"]
        warm = {v["monitor"]: v["layout"]["frames"] for v in s.status()["views"]}
        wait_for(lambda: all(v["layout"]["frames"] >= warm[v["monitor"]] + 3 for v in s.status()["views"]))
        counts = {v["monitor"]: v["layout"] for v in s.status()["views"]}
        captures = s.status()["resources"]["captures"]
        magnified = tile(s, 11)
        other_outputs = {key: box for key, box in tiles(s).items() if key[0] != monitor}
        s.button(1, PAN_BUTTON)
        wait_for(lambda: s.status()["zoom"]["panning"])
        overview_cursor(s, wait_for, "grabbing")
        assert not s.status()["dragging"]
        pointer = s.data("cursorpos")
        s.motion(23, 17)
        moved_pointer = s.data("cursorpos")
        moved = tile(s, 11)
        assert abs(moved["x"] - magnified["x"] - moved_pointer["x"] + pointer["x"]) < 0.1
        assert abs(moved["y"] - magnified["y"] - moved_pointer["y"] + pointer["y"]) < 0.1
        assert abs(moved["w"] - magnified["w"]) < 0.1 and abs(moved["h"] - magnified["h"]) < 0.1
        wheel(s, -4)
        for button in (272, 274):
            s.button(1, button)
            s.button(0, button)
        assert same_geometry({(monitor, 11): moved}, {(monitor, 11): tile(s, 11)})
        assert s.status()["live"] and not s.status()["dragging"]
        assert abs(s.status()["zoom"]["extra_goal"] - 1.15 ** 2) < 0.0001

        relative_to(s, (work["x"] + work["w"] - 70, work["y"] + work["h"] - 70))
        positive = tile(s, 11)
        assert abs(positive["x"] - fitted["x"]) < 0.1 and abs(positive["y"] - fitted["y"]) < 0.1
        assert inside(fitted, positive, 0.1)
        s.motion(20, 20)
        assert same_geometry({(monitor, 11): positive}, {(monitor, 11): tile(s, 11)})
        pointer = s.data("cursorpos")
        s.motion(-8, -6)
        moved_pointer = s.data("cursorpos")
        reversed_box = tile(s, 11)
        assert abs(reversed_box["x"] - positive["x"] - moved_pointer["x"] + pointer["x"]) < 0.1
        assert abs(reversed_box["y"] - positive["y"] - moved_pointer["y"] + pointer["y"]) < 0.1

        relative_to(s, (work["x"] + 70, work["y"] + 70))
        negative = tile(s, 11)
        assert abs(negative["x"] + negative["w"] - fitted["x"] - fitted["w"]) < 0.1
        assert abs(negative["y"] + negative["h"] - fitted["y"] - fitted["h"]) < 0.1
        s.motion(-20, -20)
        assert same_geometry({(monitor, 11): negative}, {(monitor, 11): tile(s, 11)})
        pointer = s.data("cursorpos")
        s.motion(8, 6)
        moved_pointer = s.data("cursorpos")
        final = tile(s, 11)
        assert abs(final["x"] - negative["x"] - moved_pointer["x"] + pointer["x"]) < 0.1
        assert abs(final["y"] - negative["y"] - moved_pointer["y"] + pointer["y"]) < 0.1
        assert inside(fitted, final, 0.1)
        assert same_geometry(other_outputs, {key: box for key, box in tiles(s).items() if key[0] != monitor})
        measured = s.status()
        assert s.geometry() == native
        assert {m["name"]: m["activeWorkspace"]["id"] for m in s.data("monitors")} == active
        assert all(measured["resources"]["captures"][key] == captures[key] for key in ("attempts", "bytes"))
        for view in measured["views"]:
            assert all(view["layout"][key] == counts[view["monitor"]][key] for key in ("grid_layouts", "window_layouts", "spread_layouts"))
        s.button(0, PAN_BUTTON)
        assert not s.status()["zoom"]["pan_held"] and not s.status()["zoom"]["panning"]
        assert s.status()["zoom"]["held"] and same_geometry({(monitor, 11): final}, {(monitor, 11): tile(s, 11)})
        overview_cursor(s, wait_for, "grab")
        s.check("right-button pan follows real mouse deltas, clamps both edges and reverses immediately without changing scale, captures, layouts or native windows")

        s.move(s.preview_point("hs-A", 0.55, 0.55))
        selected = s.status()["target"]
        expected = s.point(s.windows()["hs-A"], 0.55, 0.55)
        assert selected["window"] == s.windows()["hs-A"]["address"]
        assert abs(selected["x"] - expected[0]) < 2.5 and abs(selected["y"] - expected[1]) < 2.5
        s.key(SUPER, 1)
        s.button(1, PAN_BUTTON)
        assert s.status()["dragging"] and s.status()["drag"]["resize"]
        assert not s.status()["zoom"]["pan_held"] and not s.status()["zoom"]["panning"]
        s.run("wtype", "-k", "Escape")
        s.button(0, PAN_BUTTON)
        s.key(SUPER, 0)
        assert s.geometry() == native
        s.key(Z, 0)
        released(s, wait_for)
        overview_cursor(s, wait_for, "default")
        assert same_geometry(grid, tiles(s))
        s.close()
        restored = overrides()
        assert restored["unknown"] == "crosshair" and restored["window_edge"] == "ew-resize", restored
        s.check("panned previews retain native hit mapping and Super-right resize; cursor changes from default through grab and grabbing and restores previous overrides")

        opened(s, wait_for)
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        s.move(center(tile(s, 11)))
        wheel(s, -2)
        inspection(s, wait_for, 1.15 ** 2)
        s.button(1, PAN_BUTTON)
        wait_for(lambda: s.status()["zoom"]["panning"])
        overview_cursor(s, wait_for, "grabbing")
        overrides("set 0 wait")
        s.button(0, PAN_BUTTON)
        s.key(Z, 0)
        released(s, wait_for)
        s.close()
        assert overrides()["unknown"] == "wait"
        s.check("pan cursor teardown preserves an external cursor replacement made while the grip was active")
    finally:
        s.button(0, PAN_BUTTON)
        s.button(0)
        s.button(0, 274)
        s.key(SUPER, 0)
        s.key(Z, 0)
        s.close()
        if saved_cursor is not None:
            restore_override(0, saved_cursor["unknown"])
            restore_override(1, saved_cursor["window_edge"])
        s.ctl("plugin", "unload", str(fixture))


def gestures(s, wait_for):
    s.setup("dwindle", 0, 1)
    primary_destinations(s)
    address = s.windows()["hs-A"]["address"]
    work = area(next(m for m in s.data("monitors") if m["name"] == s.names[0]))
    s.ctl("dispatch", "setfloating", "address:" + address)
    s.ctl("dispatch", "resizewindowpixel", f"exact 240 160,address:{address}")
    s.ctl("dispatch", "movewindowpixel", f"exact {work['x'] + 160:.0f} {work['y'] + 160:.0f},address:{address}")
    opened(s, wait_for)
    for button in (272, 273):
        s.key(Z, 1)
        wait_for(lambda: s.status()["zoom"]["held"])
        inspection(s, wait_for, 1)
        s.move(center(tile(s, 11)))
        wheel(s, -2)
        inspection(s, wait_for, 1.15 ** 2)
        s.move(s.preview_point("hs-A", 0.8, 0.8))
        original = s.geometry()
        s.key(SUPER, 1)
        s.button(1, button)
        assert s.status()["dragging"]
        frozen = tile(s, 11)
        wheel(s, -3)
        assert same_geometry({(s.names[0], 11): frozen}, {(s.names[0], 11): tile(s, 11)})
        s.move(edge_point(work, "right"))
        time.sleep(0.4)
        assert s.status()["zoom"]["workspace"] == 11 and not zoom_edges(s, s.names[0])
        s.key(Z, 0)
        released(s, wait_for)
        assert s.status()["dragging"] and s.geometry() == original
        s.move(s.preview_point("hs-A", 0.8, 0.8))
        s.run("wtype", "-k", "Escape")
        s.button(0, button)
        s.key(SUPER, 0)
        assert not s.status()["dragging"] and s.geometry() == original
    s.check("move and resize freeze wheel inspection, ignore additional wheel input and return to the grid on Z release")

    s.key(Z, 1)
    wait_for(lambda: s.status()["zoom"]["held"])
    inspection(s, wait_for, 1)
    s.move(center(tile(s, 11)))
    wheel(s, -2)
    inspection(s, wait_for, 1.15 ** 2)
    s.move(s.preview_point("hs-A"))
    s.key(SUPER, 1)
    s.button(1)
    s.key(Z, 0)
    released(s, wait_for)
    s.move(center(tile(s, 21)))
    s.button(0)
    s.key(SUPER, 0)
    wait_for(lambda: s.windows()["hs-A"]["workspace"]["id"] == 21)
    assert not s.status()["dragging"] and s.status()["layout_targets_unique"]
    s.check("a move released after zoom-out drops into the tile currently under the pointer")
    s.close()
    s.ctl("dispatch", "movetoworkspacesilent", f"11,address:{address}")
    s.ctl("dispatch", "focuswindow", "address:" + address)
    opened(s, wait_for)

    s.key(Z, 1)
    wait_for(lambda: s.status()["zoom"]["held"])
    inspection(s, wait_for, 1)
    s.move(center(tile(s, 11)))
    wheel(s, -2)
    inspection(s, wait_for, 1.15 ** 2)
    s.move(s.preview_point("hs-A", 0.8, 0.8))
    start = s.data("cursorpos")
    preview = s.preview("hs-A")
    initial = s.windows()["hs-A"]["size"]
    delta = 24
    expected = (initial[0] + delta * initial[0] / preview["w"], initial[1] + delta * initial[1] / preview["h"])
    s.key(SUPER, 1)
    s.button(1, 273)
    s.move((start["x"] + delta, start["y"] + delta))
    s.key(Z, 0)
    released(s, wait_for)
    s.button(0, 273)
    s.key(SUPER, 0)
    wait_for(lambda: not s.status()["dragging"])
    wait_for(lambda: s.windows()["hs-A"]["size"] != initial)
    actual = s.windows()["hs-A"]
    assert all(abs(value - target) <= 3 for value, target in zip(actual["size"], expected)), (actual["size"], expected)
    assert actual["workspace"]["id"] == 11 and inside(window_box(actual), work)
    s.check("a resize released after zoom-out retains its original pickup scale and source workspace")
    s.close()


def transitions(s, wait_for):
    s.ctl("keyword", "animations:enabled", "true")
    s.ctl("keyword", "animation", "windowsMove,1,12,default")
    primary_destinations(s)
    try:
        opened(s, wait_for)
        time.sleep(1.4)
        baseline = tiles(s)
        start = tile(s, 11)
        s.key(Z, 1)
        wait_for(lambda: tile(s, 11)["w"] > start["w"] + 15)
        intermediate = tile(s, 11)["w"]
        s.key(Z, 0)
        assert not s.status()["zoom"]["held"]
        wait_for(lambda: tile(s, 11)["w"] < intermediate - 5)
        released(s, wait_for)
        assert same_geometry(baseline, tiles(s))

        # Use the currently visible neighbor during the return, rather than
        # assuming the final grid coordinates already match what is drawn.
        s.key(102, 1)
        s.key(102, 0)
        s.move(center(tile(s, 11)))
        work = area(next(m for m in s.data("monitors") if m["name"] == s.names[0]))
        s.key(Z, 1)
        wait_for(lambda: zoom_edges(s, s.names[0]))
        s.key(Z, 0)

        def visible_neighbor_point():
            box = tile(s, 21)
            left, top = max(box["x"], work["x"]), max(box["y"], work["y"])
            right = min(box["x"] + box["w"], work["x"] + work["w"])
            bottom = min(box["y"] + box["h"], work["y"] + work["h"])
            if right - left > 32 and bottom - top > 32:
                return (left + right) / 2, (top + bottom) / 2
            return None

        s.move(wait_for(visible_neighbor_point))
        state = s.status()
        assert state["zoom"]["returning"], state["zoom"]
        assert state["target"]["workspace"] == 21, state["target"]
        s.key(Z, 1)
        assert s.status()["zoom"]["workspace"] == 21
        s.key(Z, 0)
        released(s, wait_for)
        s.check("release, move to a visible neighbor and repress Z selects it before zoom-out finishes")

        # A single movement can finish while the old enlarged preview still
        # occupies the intended grid location. Follow the returning geometry
        # without requiring another motion event once the grid is restored.
        s.key(102, 1)
        s.key(102, 0)
        s.move(center(tile(s, 11)))
        s.key(Z, 1)
        wait_for(lambda: zoom_edges(s, s.names[0]))
        s.key(Z, 0)
        s.move(center(baseline[(s.names[0], 21)]))
        assert s.status()["zoom"]["returning"]
        pointer = s.data("cursorpos")
        released(s, wait_for)
        assert s.status()["target"]["workspace"] == 21
        assert s.data("cursorpos") == pointer
        s.key(Z, 1)
        assert s.status()["zoom"]["workspace"] == 21
        wait_for(lambda: zoom_edges(s, s.names[0]))
        s.move(center(baseline[(s.names[0], 22)]))
        s.key(Z, 0)
        released(s, wait_for)
        assert s.status()["target"]["workspace"] == 21
        s.check("post-release motion follows returning tiles without a wiggle; stationary release retains selection")

        s.key(Z, 1)
        wait_for(lambda: zoom_edges(s, s.names[0]))
        s.move(center(baseline[(s.names[0], 22)]))
        pointer = s.data("cursorpos")
        namespace = "waybar-zoom-return"
        panel = s.spawn(["python3", str(s.artifact("layer.py")), str(s.root / "zoom-return-panel"), namespace, "bottom"])
        try:
            wait_for(lambda: s.layer(namespace) and not s.status()["cursor_owned"])
            assert s.status()["zoom"]["held"]
            s.key(Z, 0)
            released(s, wait_for)
            panel.terminate()
            panel.wait(timeout=5)
            wait_for(lambda: not s.layer(namespace) and s.status()["cursor_owned"])
            wait_for(lambda: same_geometry(baseline, tiles(s)))
            assert s.data("cursorpos") == pointer
            assert s.status()["target"]["workspace"] == 21
            s.check("stationary panel restoration after zoom-out does not resume pointer selection")
        finally:
            if panel.poll() is None:
                panel.terminate()
                panel.wait(timeout=5)

        s.ctl("keyword", "plugin:hyprspace:follow_mouse", "false")
        try:
            s.key(102, 1)
            s.key(102, 0)
            s.move(center(tile(s, 11)))
            s.key(Z, 1)
            wait_for(lambda: tile(s, 11)["w"] > baseline[(s.names[0], 11)]["w"] * 2)
            time.sleep(1.4)
            s.key(Z, 0)
            s.move(wait_for(visible_neighbor_point))
            assert s.status()["zoom"]["returning"]
            assert s.status()["target"]["workspace"] == 11
            s.key(Z, 1)
            assert s.status()["zoom"]["workspace"] == 11
            s.key(Z, 0)
            released(s, wait_for)
            s.check("disabled follow_mouse retains keyboard selection during rapid zoom release and repress")
        finally:
            s.ctl("keyword", "plugin:hyprspace:follow_mouse", "true")

        s.key(102, 1)
        s.key(102, 0)
        for button in (272, 273):
            s.key(Z, 1)
            time.sleep(1.4)
            s.key(Z, 0)
            time.sleep(0.08)
            assert s.status()["zoom"]["returning"]
            s.move(s.preview_point("hs-A", 0.5, 0.5))
            s.key(SUPER, 1)
            s.button(1, button)
            assert s.status()["dragging"]
            released(s, wait_for)
            assert same_geometry(baseline, tiles(s)), "drag pickup froze an unfinished zoom return"
            s.run("wtype", "-k", "Escape")
            s.button(0, button)
            s.key(SUPER, 0)
        s.check("move and resize pickup during zoom-out allow the camera to finish returning")
        s.key(Z, 1)
        time.sleep(1.4)
        s.key(TAB, 1)
        s.key(TAB, 0)
        time.sleep(0.08)
        before = tile(s, 21)
        s.key(28, 1)  # Enter during the workspace pan.
        s.key(28, 0)
        assert not s.status()["live"]
        after = tile(s, 21)
        monitor = next(m for m in s.data("monitors") if m["name"] == s.names[0])
        assert all(abs(after[part] - before[part]) < monitor["width"] * 0.15 for part in ("x", "y", "w", "h")), (before, after)
        s.key(Z, 0)
        wait_for(lambda: not s.status()["views"])
        assert s.data("activeworkspace")["id"] == 21
        assert all(window["alpha"] == 1 for window in s.status()["windows"])
        s.check("zoom reverses before settling and Enter closes smoothly during workspace panning")

        # Reanchor while the ordinary opening animation is still in flight.
        s.ctl("dispatch", "workspace", "11")
        s.ctl("dispatch", "hyprspace:overview", "on")
        s.key(Z, 1)
        s.key(TAB, 1)
        s.key(TAB, 0)
        time.sleep(0.08)
        selected = s.status()["target"]["workspace"]
        before = tile(s, selected)
        s.key(28, 1)
        s.key(28, 0)
        after = tile(s, selected)
        assert all(abs(after[part] - before[part]) < monitor["width"] * 0.15 for part in ("x", "y", "w", "h")), (before, after)
        s.key(Z, 0)
        wait_for(lambda: not s.status()["views"])
        assert all(window["alpha"] == 1 for window in s.status()["windows"])
        s.check("closing during opening and held zoom preserves displayed geometry and restores visibility")
    finally:
        s.key(Z, 0)
        s.ctl("keyword", "animations:enabled", "false")
        s.close()


def wheel_transitions(s, wait_for):
    fixture = load_overview_fixture(s, wait_for)
    try:
        s.setup("dwindle", 0, 1)
        s.ctl("keyword", "animations:enabled", "true")
        s.ctl("keyword", "animation", "windowsMove,1,12,default")
        primary_destinations(s)
        opened(s, wait_for)
        time.sleep(1.4)
        grid = tiles(s)
        assert len(tiles(s, s.names[0])) == 10, tiles(s, s.names[0])
        s.move(center(tile(s, 11)))
        s.key(Z, 1)
        assert s.status()["zoom"]["inspection_ready"] and not s.status()["zoom"]["camera_settled"]
        wheel(s, 4)
        assert s.status()["zoom"]["extra_goal"] == 1
        assert not s.status()["zoom"]["camera_settled"], "wheel-out at minimum must leave the initial fit running"
        before = tile(s, 11)
        pointer = s.data("cursorpos")
        assert before["x"] <= pointer["x"] < before["x"] + before["w"] and before["y"] <= pointer["y"] < before["y"] + before["h"], (before, pointer)
        s.ctl("dispatch", "hyprspace-test:axis-continuity", "0 0 -60 -480 0")
        assert abs(s.status()["zoom"]["extra_goal"] - 1.15 ** 4) < 0.0001, s.status()["zoom"]
        inspection(s, wait_for, 1.15 ** 4)
        wheel(s, 32)
        inspection(s, wait_for, 1)
        fitted = tile(s, 11)
        s.move(center(fitted))
        pointer = s.data("cursorpos")
        anchor = ((pointer["x"] - fitted["x"]) / fitted["w"], (pointer["y"] - fitted["y"]) / fitted["h"])
        wheel(s, -4)
        wait_for(lambda: 1.05 < s.status()["zoom"]["extra_factor"] < 1.15 ** 4 - 0.02)
        for _ in range(4):
            state = s.status()
            current = next(t for v in state["views"] for t in v["tiles"] if t["workspace"] == 11 and t["window"] == "0x0")
            assert 1 <= state["zoom"]["extra_factor"] <= 4
            assert abs(current["x"] + anchor[0] * current["w"] - pointer["x"]) < 0.2
            assert abs(current["y"] + anchor[1] * current["h"] - pointer["y"]) < 0.2
            time.sleep(0.025)
        wheel(s, -1)
        assert abs(s.status()["zoom"]["extra_goal"] - 1.15 ** 5) < 0.0001
        wheel(s, 1)
        reversal = s.status()["zoom"]
        assert reversal["extra_goal"] < reversal["extra_factor"]
        inspection(s, wait_for)
        wheel(s, -4)
        wait_for(lambda: s.status()["zoom"]["extra_factor"] > 1.5 and s.status()["zoom"]["inspection_transitioning"])
        before = tile(s, 11)
        s.key(Z, 0)
        after = tile(s, 11)
        assert s.status()["zoom"]["extra_factor"] == s.status()["zoom"]["extra_goal"] == 1
        assert abs(after["w"] / before["w"] - 1) < 0.06, (before, after)
        assert abs(after["h"] / before["h"] - 1) < 0.06, (before, after)
        released(s, wait_for)
        assert same_geometry(grid, tiles(s))
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        assert same_geometry({(s.names[0], 11): fitted}, {(s.names[0], 11): tile(s, 11)})
        s.move(center(fitted))
        wheel(s, -4)
        wait_for(lambda: s.status()["zoom"]["extra_factor"] > 1.5)
        s.move(edge_point(area(next(m for m in s.data("monitors") if m["name"] == s.names[0])), "right"))
        wheel(s, 32)
        assert s.status()["zoom"]["inspection_transitioning"] and not zoom_edges(s, s.names[0])
        inspection(s, wait_for, 1)
        time.sleep(0.4)
        assert s.status()["zoom"]["workspace"] == 11
        s.check("initial fit accepts wheel input immediately; minimum preserves fit motion and lens bursts reverse without a Z-release jump or stale edge dwell")

        # Hold the takeover's first sample so pickup deterministically occurs
        # before the selected workspace reaches its ordinary held fit.
        s.key(Z, 0)
        released(s, wait_for)
        native = s.geometry()
        for button in (272, 273):
            s.move(center(tile(s, 11)))
            s.key(Z, 1)
            wheel(s, -4)
            assert abs(s.status()["zoom"]["extra_goal"] - 1.15 ** 4) < 0.0001
            s.ctl("dispatch", "hyprspace-test:inspection-progress", "0")
            early = tile(s, 11)
            assert early["w"] < fitted["w"]
            s.move(s.preview_point("hs-A", 0.5, 0.5))
            s.key(SUPER, 1)
            s.button(1, button)
            assert s.status()["dragging"]
            frozen = tile(s, 11)
            wheel(s, -4)
            assert same_geometry({(s.names[0], 11): frozen}, {(s.names[0], 11): tile(s, 11)})
            s.run("wtype", "-k", "Escape")
            s.button(0, button)
            s.key(SUPER, 0)
            assert not s.status()["dragging"] and s.geometry() == native
            wait_for(lambda: abs(tile(s, 11)["w"] - fitted["w"]) < 0.1)
            inspection(s, wait_for, 1)
            s.key(Z, 0)
            released(s, wait_for)
            assert same_geometry(grid, tiles(s))
        s.check("move and resize freeze an early wheel takeover; cancellation refits without stranding the camera below the held baseline")

        for button in (272, 273):
            s.move(center(tile(s, 11)))
            s.key(Z, 1)
            inspection(s, wait_for, 1)
            s.key(TAB, 1)
            s.key(TAB, 0)
            time.sleep(0.08)
            s.key(SHIFT, 1)
            s.key(TAB, 1)
            s.key(TAB, 0)
            s.key(SHIFT, 0)
            assert s.status()["zoom"]["workspace"] == 11
            s.move(center(tile(s, 11)))
            wheel(s, -4)
            assert abs(s.status()["zoom"]["extra_goal"] - 1.15 ** 4) < 0.0001
            s.ctl("dispatch", "hyprspace-test:inspection-progress", "0")
            early = tile(s, 11)
            assert abs(early["w"] - fitted["w"]) < 0.1
            assert abs(early["x"] - fitted["x"]) + abs(early["y"] - fitted["y"]) > 1
            s.move(s.preview_point("hs-A", 0.5, 0.5))
            s.key(SUPER, 1)
            s.button(1, button)
            assert s.status()["dragging"]
            s.run("wtype", "-k", "Escape")
            s.button(0, button)
            s.key(SUPER, 0)
            assert not s.status()["dragging"] and s.geometry() == native
            wait_for(lambda: same_geometry({(s.names[0], 11): fitted}, {(s.names[0], 11): tile(s, 11)}))
            inspection(s, wait_for, 1)
            s.key(Z, 0)
            released(s, wait_for)
            assert same_geometry(grid, tiles(s))
        s.check("move and resize cancellation restore position after an early wheel takeover of a same-scale workspace pan")

        s.move(center(tile(s, 11)))
        s.key(Z, 1)
        wheel(s, -2)
        inspection(s, wait_for, 1.15 ** 2)
        s.key(Z, 0)
        time.sleep(0.04)
        s.move(center(tile(s, 11)))
        s.key(Z, 1)
        assert s.status()["zoom"]["inspection_ready"] and not s.status()["zoom"]["camera_settled"]
        wheel(s, -1)
        assert abs(s.status()["zoom"]["extra_goal"] - 1.15) < 0.0001
        inspection(s, wait_for, 1.15)
        s.check("rapid release and repress accepts the first wheel immediately using the new hold's zoom goal")

        s.move(center(fitted))
        wheel(s, -4)
        wait_for(lambda: s.status()["zoom"]["extra_factor"] > 1.5)
        s.move(s.preview_point("hs-A", 0.5, 0.5))
        s.key(SUPER, 1)
        s.button(1)
        assert s.status()["dragging"]
        frozen = tile(s, 11)
        wheel(s, -4)
        time.sleep(0.12)
        assert same_geometry({(s.names[0], 11): frozen}, {(s.names[0], 11): tile(s, 11)})
        s.key(Z, 0)
        released(s, wait_for)
        assert same_geometry(grid, tiles(s)) and s.status()["dragging"]
        s.run("wtype", "-k", "Escape")
        s.button(0)
        s.key(SUPER, 0)
        s.check("drag pickup during wheel animation freezes its displayed camera and Z release still finishes returning to the grid")
        s.close()

        opened(s, wait_for)
        time.sleep(1.4)
        grid = tiles(s)
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        s.move(center(tile(s, 11)))
        wheel(s, -4)
        inspection(s, wait_for, 1.15 ** 4)
        wheel(s, 32)
        s.ctl("dispatch", "hyprspace-test:inspection-progress", "1.1")
        assert s.status()["zoom"]["extra_factor"] == 1
        s.key(Z, 0)
        released(s, wait_for)
        assert same_geometry(grid, tiles(s))
        s.ctl("dispatch", "hyprspace-test:inspection-progress", "0.9")
        assert same_geometry(grid, tiles(s)), "late nonmonotonic progress revived cancelled inspection"
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        s.ctl("dispatch", "hyprspace-test:inspection-progress", "0.8")
        assert s.status()["zoom"]["extra_factor"] == 1
        s.key(Z, 0)
        released(s, wait_for)
        s.check("cancelled overshooting inspection cannot revive when a later progress sample dips below one or a new Z hold begins")
        s.close()

        opened(s, wait_for)
        s.key(Z, 1)
        assert s.status()["zoom"]["inspection_ready"] and not s.status()["zoom"]["camera_settled"]
        wheel(s, -4)
        assert abs(s.status()["zoom"]["extra_goal"] - 1.15 ** 4) < 0.0001
        inspection(s, wait_for, 1.15 ** 4)
        s.move(center(tile(s, 11)))
        wheel(s, -4)
        wait_for(lambda: s.status()["zoom"]["extra_factor"] > 1.8 and s.status()["zoom"]["inspection_transitioning"])
        before = tile(s, 11)
        s.key(28, 1)
        s.key(28, 0)
        after = tile(s, 11)
        assert abs(after["w"] / before["w"] - 1) < 0.08, (before, after)
        s.key(Z, 0)
        wait_for(lambda: not s.status()["views"])
        assert all(window["alpha"] == 1 for window in s.status()["windows"])
        s.check("opening accepts immediate magnification and closing mid-inspection preserves displayed geometry and restores visibility")
    finally:
        s.key(Z, 0)
        s.button(0)
        s.key(SUPER, 0)
        s.ctl("keyword", "animations:enabled", "false")
        s.close()
        s.ctl("plugin", "unload", str(fixture))


def pan_transitions(s, wait_for):
    fixture = load_overview_fixture(s, wait_for)
    try:
        s.setup("dwindle", 0, 1)
        primary_destinations(s)
        s.ctl("keyword", "animations:enabled", "true")
        s.ctl("keyword", "animation", "windowsMove,1,12,default")
        opened(s, wait_for)
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        fitted = tile(s, 11)
        s.key(Z, 0)
        released(s, wait_for)
        grid = tiles(s)

        s.move(center(tile(s, 11)))
        s.key(Z, 1)
        wheel(s, -4)
        s.ctl("dispatch", "hyprspace-test:pan-continuity", "0")
        state = s.status()
        assert state["zoom"]["pan_held"] and not state["zoom"]["panning"] and state["live"]
        assert tile(s, 11)["w"] < fitted["w"]
        s.motion(7, 5)
        wait_for(lambda: s.status()["zoom"]["panning"])
        overview_cursor(s, wait_for, "grabbing")
        frozen = tile(s, 11)
        assert frozen["w"] > fitted["w"] and inside(fitted, frozen, 0.1)
        pointer = s.data("cursorpos")
        s.motion(5, 4)
        after = tile(s, 11)
        moved_pointer = s.data("cursorpos")
        for part, size in (("x", "w"), ("y", "h")):
            expected = min(fitted[part], max(fitted[part] + fitted[size] - frozen[size], frozen[part] + moved_pointer[part] - pointer[part]))
            assert abs(after[part] - expected) < 0.1, (frozen, after, pointer, moved_pointer)
        s.button(0, PAN_BUTTON)
        wheel(s, 32)
        inspection(s, wait_for, 1)
        assert same_geometry({(s.names[0], 11): fitted}, {(s.names[0], 11): tile(s, 11)})
        s.check("an early right-button grip waits for a valid magnified fit, then pans without accumulating movement from the pending fit")

        s.move(center(fitted))
        wheel(s, -4)
        s.ctl("dispatch", "hyprspace-test:pan-continuity", "0.45")
        state = s.status()
        assert state["zoom"]["panning"] and state["zoom"]["pan_held"]
        assert 1 < state["zoom"]["extra_factor"] < 1.15 ** 4
        assert not state["zoom"]["inspection_transitioning"]
        frozen = tile(s, 11)
        frames = next(v["layout"]["frames"] for v in s.status()["views"] if v["monitor"] == s.names[0])
        wait_for(lambda: next(v["layout"]["frames"] for v in s.status()["views"] if v["monitor"] == s.names[0]) >= frames + 3)
        assert same_geometry({(s.names[0], 11): frozen}, {(s.names[0], 11): tile(s, 11)})
        s.button(0, PAN_BUTTON)
        assert same_geometry({(s.names[0], 11): frozen}, {(s.names[0], 11): tile(s, 11)})
        s.key(Z, 0)
        released(s, wait_for)
        assert same_geometry(grid, tiles(s))
        s.check("right-button pickup freezes a magnifying animation without a synchronous preview jump and release preserves that exact camera")

        s.move(center(tile(s, 11)))
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        s.move(center(tile(s, 11)))
        wheel(s, -2)
        inspection(s, wait_for, 1.15 ** 2)
        s.button(1, PAN_BUTTON)
        wait_for(lambda: s.status()["zoom"]["panning"])
        s.motion(12, 9)
        s.key(Z, 0)
        released(s, wait_for)
        assert not s.status()["zoom"]["pan_held"] and not s.status()["zoom"]["panning"]
        assert same_geometry(grid, tiles(s))
        overview_cursor(s, wait_for, "default")
        s.button(0, PAN_BUTTON)
        assert s.status()["live"] and same_geometry(grid, tiles(s))
        s.check("releasing Z before the right button returns to the exact grid and consumes the late right-button release")

        s.move(center(tile(s, 11)))
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        s.move(center(tile(s, 11)))
        wheel(s, -2)
        inspection(s, wait_for, 1.15 ** 2)
        s.button(1, PAN_BUTTON)
        wait_for(lambda: s.status()["zoom"]["panning"])
        s.key(TAB, 1)
        s.key(TAB, 0)
        inspection(s, wait_for, 1)
        assert s.status()["zoom"]["workspace"] == 21
        assert not s.status()["zoom"]["pan_held"] and not s.status()["zoom"]["panning"]
        s.button(1, PAN_BUTTON)
        assert not s.status()["zoom"]["pan_held"], "a repeated held press rearmed a cancelled pan grip"
        s.button(0, PAN_BUTTON)
        assert s.status()["live"]
        s.key(102, 1)
        s.key(102, 0)
        inspection(s, wait_for, 1)
        s.move(center(tile(s, 11)))
        wheel(s, -2)
        inspection(s, wait_for, 1.15 ** 2)
        s.button(1, PAN_BUTTON)
        wait_for(lambda: s.status()["zoom"]["panning"])
        s.ctl("keyword", "monitor", f"{s.names[0]},addreserved,10,10,10,10")
        wait_for(lambda: not s.status()["zoom"]["pan_held"] and not s.status()["zoom"]["panning"])
        inspection(s, wait_for, 1)
        s.button(0, PAN_BUTTON)
        assert s.status()["live"]
        s.ctl("keyword", "monitor", f"{s.names[0]},addreserved,0,0,0,0")
        inspection(s, wait_for, 1)
        s.key(Z, 0)
        released(s, wait_for)
        s.check("workspace retargeting and fit-bound changes cancel the grip and retain its release lease until a fresh right-button press")

        s.move(center(tile(s, 11)))
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        s.move(center(tile(s, 11)))
        wheel(s, -2)
        inspection(s, wait_for, 1.15 ** 2)
        s.button(1, PAN_BUTTON)
        wait_for(lambda: s.status()["zoom"]["panning"])
        s.ctl("reload")
        wait_for(lambda: not s.status()["zoom"]["held"] and not s.status()["zoom"]["pan_held"] and not s.status()["zoom"]["panning"])
        overview_cursor(s, wait_for, "default")
        s.button(0, PAN_BUTTON)
        s.key(Z, 1)
        assert not s.status()["zoom"]["held"]
        s.key(Z, 0)
        assert s.status()["live"] and s.ctl("configerrors") == ""
        s.check("a private config reload cancels active panning, restores its cursor and consumes old held-key and button releases")
    finally:
        s.button(0, PAN_BUTTON)
        s.key(Z, 0)
        s.ctl("keyword", "monitor", f"{s.names[0]},addreserved,0,0,0,0")
        s.ctl("keyword", "animations:enabled", "false")
        s.close()
        s.ctl("plugin", "unload", str(fixture))


def pan_input_lifecycle(s, wait_for):
    from regressions import events, offsets

    fixture = load_overview_fixture(s, wait_for)
    secondary = None
    panel = None
    disabled_name = None
    device_commands = []

    def other_button(state, button=PAN_BUTTON):
        secondary.stdin.write(f"{button} {state}\n")
        secondary.stdin.flush()
        assert reply(secondary) == "ok"

    def magnify():
        opened(s, wait_for)
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        s.move(center(tile(s, 11)))
        wheel(s, -2)
        inspection(s, wait_for, 1.15 ** 2)

    def native_click():
        saved = offsets(s)
        s.button(1, PAN_BUTTON)
        s.button(0, PAN_BUTTON)
        s.motion(1, 0)
        wait_for(lambda: len(events(saved, r"wl_pointer#\d+\.button\(")) == 2)
        assert not s.status()["live"]

    def pointer_states():
        return json.loads(s.run("hyprctl", "dispatch", "hyprspace-test:pointer-state"))

    def device_command(arguments, replies=1):
        record = {"arguments": arguments, "before": pointer_states()}
        device_commands.append(record)
        record["reply"] = s.run("hyprctl", *arguments)
        record["after"] = pointer_states()
        if replies == 1:
            assert record["reply"] == "ok", record
        else:
            assert [line for line in record["reply"].splitlines() if line] == ["ok"] * replies, record

    def matching_pointer(name, identity):
        return next(pointer for pointer in pointer_states() if pointer["name"] == name and pointer["identity"] == identity)

    s.env["WAYLAND_DEBUG"] = "client"
    try:
        s.setup("dwindle", 0, 1)
    finally:
        s.env.pop("WAYLAND_DEBUG")
    try:
        primary_destinations(s)
        secondary = s.spawn([str(s.artifact("test-pointer"))], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        assert reply(secondary) == "ready"
        magnify()
        s.button(1, PAN_BUTTON)
        wait_for(lambda: s.status()["zoom"]["panning"])
        frozen = tile(s, 11)
        other_button(0)
        assert not s.status()["zoom"]["pan_held"] and not s.status()["zoom"]["panning"]
        s.button(0, PAN_BUTTON)
        assert s.status()["live"] and same_geometry({(s.names[0], 11): frozen}, {(s.names[0], 11): tile(s, 11)})
        s.key(Z, 0)
        released(s, wait_for)
        s.close()
        s.check("the first right-button release from either pointer ends the aggregate grip without a late-release action")

        s.move(s.point(s.windows()["hs-A"]))
        saved = offsets(s)
        other_button(1)
        wait_for(lambda: len(events(saved, r"wl_pointer#\d+\.button\(")) == 1)
        magnify()
        s.button(1, PAN_BUTTON)
        assert not s.status()["zoom"]["pan_held"] and not s.status()["zoom"]["panning"]
        assert s.status()["live"]
        s.button(0, PAN_BUTTON)
        other_button(0)
        s.key(Z, 0)
        released(s, wait_for)
        s.close()
        s.check("a native right-button hold on another pointer prevents a new overview pan takeover")

        magnify()
        s.button(1, PAN_BUTTON)
        wait_for(lambda: s.status()["zoom"]["panning"])
        s.close()
        s.key(Z, 0)
        s.move(s.point(s.windows()["hs-A"]))
        saved = offsets(s)
        s.button(0, PAN_BUTTON)
        s.motion(1, 0)
        wait_for(lambda: events(saved, r"wl_pointer#\d+\.motion\("))
        assert not events(saved, r"wl_pointer#\d+\.button\("), events(saved, r"wl_pointer#")
        native_click()
        s.check("closing during a right-button grip swallows its late release and the next native click receives a full press-release pair")

        magnify()
        other_button(1)
        wait_for(lambda: s.status()["zoom"]["panning"])
        secondary.terminate()
        secondary.wait(timeout=3)
        secondary = None
        wait_for(lambda: not s.status()["zoom"]["pan_held"] and not s.status()["zoom"]["panning"])
        s.button(0, PAN_BUTTON)
        assert s.status()["live"] and s.status()["zoom"]["held"]
        s.key(Z, 0)
        released(s, wait_for)
        s.close()
        s.move(s.point(s.windows()["hs-A"]))
        native_click()
        s.check("pointer removal cancels an active grip, consumes an orphaned release and preserves the next native click")

        secondary = s.spawn([str(s.artifact("test-pointer"))], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        assert reply(secondary) == "ready"
        magnify()
        other_button(1)
        wait_for(lambda: s.status()["zoom"]["panning"])
        s.close()
        s.key(Z, 0)
        secondary.terminate()
        secondary.wait(timeout=3)
        secondary = None
        s.move(s.point(s.windows()["hs-A"]))
        native_click()
        s.check("removing the held pointer after overview close retires its orphaned lease before the next native right-button press")

        connected = [pointer for pointer in pointer_states() if pointer["connected"]]
        assert len(connected) == 1, pointer_states()
        name = connected[0]["name"]
        identity = connected[0]["identity"]
        selector = f"device[{name}]:enabled"
        try:
            for close_before in (False, True):
                magnify()
                s.button(1, PAN_BUTTON)
                wait_for(lambda: s.status()["zoom"]["panning"])
                if close_before:
                    s.close()
                    s.key(Z, 0)
                disabled_name = name
                device_command(("keyword", selector, "false"))
                wait_for(lambda: not matching_pointer(name, identity)["connected"])
                disabled = matching_pointer(name, identity)
                assert disabled["has_config"] and disabled["enabled_explicit"] and disabled["enabled"] == 0, disabled
                if not close_before:
                    wait_for(lambda: not s.status()["zoom"]["pan_held"] and not s.status()["zoom"]["panning"])
                device_command(("keyword", selector, "true"))
                wait_for(lambda: matching_pointer(name, identity)["connected"])
                assert matching_pointer(name, identity)["enabled"] == 1
                disabled_name = None
                if not close_before:
                    s.button(0, PAN_BUTTON)
                    s.key(Z, 0)
                    s.close()
                s.move(s.point(s.windows()["hs-A"]))
                native_click()
            s.check("disabling and reconnecting the same captured pointer clears active and closed-overview leases before a fresh native right-button cycle")

            magnify()
            s.button(1, PAN_BUTTON)
            wait_for(lambda: s.status()["zoom"]["panning"])
            frozen = tile(s, 11)
            native = s.geometry()
            before = matching_pointer(name, identity)
            device_command(("--batch", f"keyword {selector} false; keyword {selector} true"), replies=2)
            after = matching_pointer(name, identity)
            assert before["connected"] and after["connected"] and after["enabled"] == 1, (before, after)
            wait_for(lambda: not s.status()["zoom"]["pan_held"] and not s.status()["zoom"]["panning"])
            assert s.status()["zoom"]["held"] and abs(s.status()["zoom"]["extra_factor"] - 1.15 ** 2) < 0.0001
            assert same_geometry({(s.names[0], 11): frozen}, {(s.names[0], 11): tile(s, 11)})
            assert s.geometry() == native
            overview_cursor(s, wait_for, "grab")
            s.button(1, PAN_BUTTON)
            wait_for(lambda: s.status()["zoom"]["pan_held"] and s.status()["zoom"]["panning"])
            overview_cursor(s, wait_for, "grabbing")
            s.button(0, PAN_BUTTON)
            assert s.status()["zoom"]["held"] and not s.status()["zoom"]["pan_held"]
            assert same_geometry({(s.names[0], 11): frozen}, {(s.names[0], 11): tile(s, 11)})
            s.key(Z, 0)
            released(s, wait_for)
            s.close()
            s.check("a single-request detach and reattach of the same pointer cancels its active grip, preserves magnification and permits a fresh inside grip")

            magnify()
            s.button(1, PAN_BUTTON)
            wait_for(lambda: s.status()["zoom"]["panning"])
            s.close()
            s.key(Z, 0)
            before = matching_pointer(name, identity)
            device_command(("--batch", f"keyword {selector} false; keyword {selector} true"), replies=2)
            after = matching_pointer(name, identity)
            assert before["connected"] and after["connected"] and after["enabled"] == 1, (before, after)
            s.move(s.point(s.windows()["hs-A"]))
            native_click()
            s.check("a fresh outside right-button press restores native ownership after an unobserved detach and reattach of the same pointer without a release")
        except Exception:
            (s.root / "pan-device-lifecycle.json").write_text(json.dumps({
                "name": name, "identity": identity, "commands": device_commands,
                "pointers": pointer_states(), "status": s.status(),
            }, indent=2))
            raise

        entry = s.root / "pan-panel"
        namespace = "waybar-pan-test"
        panel = s.spawn(["python3", str(s.artifact("layer.py")), str(entry), namespace, "bottom"])
        wait_for(lambda: s.layer(namespace))
        magnify()
        s.button(1, PAN_BUTTON)
        wait_for(lambda: s.status()["zoom"]["panning"])
        box = s.layer(namespace)
        relative_to(s, (box["x"] + 100, box["y"] + 80))
        wait_for(lambda: not s.status()["cursor_owned"])
        assert not s.status()["zoom"]["pan_held"] and not s.status()["zoom"]["panning"]
        assert s.status()["zoom"]["held"] and abs(s.status()["zoom"]["extra_factor"] - 1.15 ** 2) < 0.0001
        s.button(0, PAN_BUTTON)
        assert not entry.with_suffix(".popup").exists()
        s.move(center(tile(s, 11)))
        wait_for(lambda: s.status()["cursor_owned"])
        overview_cursor(s, wait_for, "grab")
        s.key(Z, 0)
        released(s, wait_for)
        s.close()
        s.check("foreground pointer handoff cancels the pan grip, retains magnification and restores the grab cursor on return")
    finally:
        if disabled_name is not None:
            assert s.run("hyprctl", "keyword", f"device[{disabled_name}]:enabled", "true") == "ok"
        if secondary is not None and secondary.poll() is None:
            other_button(0)
            secondary.terminate()
            secondary.wait(timeout=3)
        if panel is not None and panel.poll() is None:
            panel.terminate()
            panel.wait(timeout=3)
        s.button(0, PAN_BUTTON)
        s.key(Z, 0)
        s.close()
        s.ctl("plugin", "unload", str(fixture))


def scrolling(s, wait_for):
    s.setup("scrolling", 0, 0)
    primary_destinations(s, "scrolling")
    opened(s, wait_for)
    s.key(Z, 1)
    inspection(s, wait_for, 1)
    s.move(center(tile(s, 11)))
    before = s.geometry()
    fitted = tile(s, 11)
    wheel(s, -3)
    inspection(s, wait_for, 1.15 ** 3)
    assert s.geometry() == before
    s.button(1, PAN_BUTTON)
    wait_for(lambda: s.status()["zoom"]["panning"])
    s.motion(13, 9)
    assert s.geometry() == before and s.status()["zoom"]["workspace"] == 11
    s.button(0, PAN_BUTTON)
    wheel(s, 32)
    inspection(s, wait_for, 1)
    assert same_geometry({(s.names[0], 11): fitted}, {(s.names[0], 11): tile(s, 11)})
    s.scroll(delta=15, discrete=0, source=1)
    wait_for(lambda: s.geometry() != before)
    assert s.status()["zoom"]["extra_factor"] == 1
    assert s.status()["zoom"]["held"] and s.status()["zoom"]["workspace"] == 11
    for axis, source in ((1, 0), (0, 3)):
        before = s.geometry()
        sign = -1 if source == 3 else 1
        s.scroll(delta=15 * sign, discrete=sign, axis=axis, source=source)
        wait_for(lambda: s.geometry() != before)
        assert s.status()["zoom"]["extra_factor"] == 1
    s.ctl("keyword", "plugin:hyprspace:overview:wheel_zoom", "false")
    before = s.geometry()
    s.scroll()
    wait_for(lambda: s.geometry() != before)
    assert s.status()["zoom"]["extra_factor"] == 1
    s.ctl("keyword", "plugin:hyprspace:overview:wheel_zoom", "true")
    s.key(Z, 0)
    released(s, wait_for)
    s.check("held wheel zoom and right-button pan leave the native scrolling tape stable; finger, horizontal, tilt and opted-out input retain tape panning")
    s.close()

    from regressions import arrow
    try:
        s.ctl("keyword", "plugin:hyprspace:overview:padding", "0")
        s.ctl("keyword", "plugin:hyprspace:overview:workspace_labels", "false")
        for direction in ("right", "down"):
            s.ctl("keyword", "scrolling:direction", direction)
            s.setup("scrolling", 0, 0)
            primary_destinations(s, "scrolling")
            s.ctl("dispatch", "focuswindow", "address:" + s.windows()["hs-A"]["address"])
            opened(s, wait_for)
            s.key(Z, 1)
            wait_for(lambda: zoom_edges(s, s.names[0]))
            box = tile(s, 11)
            point = arrow(box, direction == "right", True)
            s.move(point)
            time.sleep(0.4)
            assert s.status()["zoom"]["workspace"] == 11
            hints = zoom_edges(s, s.names[0])
            hint = next(h for h in hints if h["direction"] == direction)
            assert not (hint["x"] <= point[0] < hint["x"] + hint["w"] and hint["y"] <= point[1] < hint["y"] + hint["h"])
            before = s.geometry()
            s.button(1)
            s.button(0)
            wait_for(lambda: s.geometry() != before)
            assert s.status()["live"] and s.status()["zoom"]["workspace"] == 11
            s.run("grim", "-s", "1", "-o", s.names[0], str(s.root / f"zoom-edge-scrolling-{direction}.png"))
            s.key(Z, 0)
            released(s, wait_for)
            s.close()
        s.check("zero-padding horizontal and vertical scrolling arrows retain hover and click ownership beside zoom hints")
    finally:
        s.key(Z, 0)
        s.close()
        s.ctl("keyword", "plugin:hyprspace:overview:padding", "56")
        s.ctl("keyword", "plugin:hyprspace:overview:workspace_labels", "true")
        s.ctl("keyword", "scrolling:direction", "right")


def transformed(s, wait_for):
    s.setup("dwindle", 1, 1)
    monitor = next(m for m in s.data("monitors") if m["name"] == s.names[1])
    assert monitor["scale"] == 1.25 and monitor["transform"] % 2 == 1
    work = area(monitor)
    try:
        for workspace in range(31, 35):
            s.ctl("keyword", "workspace", f"{workspace},monitor:{s.names[1]},persistent:true,layout:dwindle")
        opened(s, wait_for, workspace=12)
        frame = next(view["layout"]["frames"] for view in s.status()["views"] if view["monitor"] == s.names[1])
        wait_for(lambda: next(view["layout"]["frames"] for view in s.status()["views"] if view["monitor"] == s.names[1]) >= frame + 3)
        baseline = tiles(s)
        assert len(tiles(s, s.names[1])) == 5
        s.key(Z, 1)
        wait_for(lambda: s.status()["zoom"]["held"])
        wait_for(lambda: tile(s, 12)["w"] > baseline[(s.names[1], 12)]["w"] * 1.5)
        inspection(s, wait_for, 1)
        enlarged = tile(s, 12)
        assert enlarged["w"] > baseline[(s.names[1], 12)]["w"] * 1.5
        assert inside(enlarged, work)
        assert abs(enlarged["w"] / enlarged["h"] - work["w"] / work["h"]) < 0.001
        assert same_geometry(
            {key: value for key, value in baseline.items() if key[0] != s.names[1]},
            {key: value for key, value in tiles(s).items() if key[0] != s.names[1]},
        )
        s.move(center(enlarged))
        wheel(s, -2)
        inspection(s, wait_for, 1.15 ** 2)
        assert inside(enlarged, tile(s, 12), 0.1)
        assert abs(tile(s, 12)["w"] / enlarged["w"] - 1.15 ** 2) < 0.0001
        s.ctl("keyword", "plugin:hyprspace:follow_mouse", "false")
        s.button(1, PAN_BUTTON)
        wait_for(lambda: s.status()["zoom"]["panning"])
        overview_cursor(s, wait_for, "grabbing")
        pointer = s.data("cursorpos")
        panned = tile(s, 12)
        s.motion(13, 9)
        moved_pointer = s.data("cursorpos")
        moved = tile(s, 12)
        assert abs(moved["x"] - panned["x"] - moved_pointer["x"] + pointer["x"]) < 0.1
        assert abs(moved["y"] - panned["y"] - moved_pointer["y"] + pointer["y"]) < 0.1
        assert abs(moved["w"] - panned["w"]) < 0.1 and abs(moved["h"] - panned["h"]) < 0.1
        assert s.status()["zoom"]["workspace"] == 12 and not s.status()["dragging"]
        s.button(0, PAN_BUTTON)
        s.ctl("keyword", "plugin:hyprspace:follow_mouse", "true")
        s.move(s.preview_point("hs-A", 0.8, 0.8))
        selected = s.status()["target"]
        assert selected["window"] == s.windows()["hs-A"]["address"]
        expected = s.point(s.windows()["hs-A"], 0.8, 0.8)
        assert abs(selected["x"] - expected[0]) < 2.5 and abs(selected["y"] - expected[1]) < 2.5, (selected, expected)
        s.key(TAB, 1)
        s.key(TAB, 0)
        assert s.status()["zoom"]["workspace"] == 31 and s.status()["zoom"]["monitor"] == s.names[1]
        inspection(s, wait_for, 1)
        assert abs(tile(s, 31)["w"] - enlarged["w"]) < 0.1
        hints = wait_for(lambda: zoom_edges(s, s.names[1]))
        destination = hints[0]["workspace"]
        s.move(center(tile(s, 31)))
        s.move(edge_point(work, hints[0]["direction"]))
        wait_for(lambda: s.status()["zoom"]["workspace"] == destination)
        time.sleep(0.4)
        assert s.status()["zoom"]["workspace"] == destination
        inspection(s, wait_for, 1)
        assert abs(tile(s, destination)["w"] - enlarged["w"]) < 0.1
        s.key(Z, 0)
        released(s, wait_for)
        assert same_geometry(baseline, tiles(s))
        s.check("multiple workspaces zoom, right-button pan and hit-test correctly on a rotated fractional-scale output even with follow_mouse disabled")
    finally:
        s.key(Z, 0)
        s.button(0, PAN_BUTTON)
        s.ctl("keyword", "plugin:hyprspace:follow_mouse", "true")
        s.close()
        for workspace in range(31, 35):
            s.ctl("keyword", "workspace", f"{workspace},persistent:false")


def wheel_teardown(s, wait_for):
    s.setup("dwindle", 0, 1)
    opened(s, wait_for)
    s.key(Z, 1)
    inspection(s, wait_for, 1)
    s.move(center(tile(s, 11)))
    wheel(s, -3)
    inspection(s, wait_for, 1.15 ** 3)
    s.button(1, PAN_BUTTON)
    wait_for(lambda: s.status()["zoom"]["panning"])
    locker = s.spawn([str(s.artifact("test-lock"))], stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)

    def request(command):
        locker.stdin.write(command.encode() + b"\n")
        locker.stdin.flush()
        return json.loads(reply(locker))

    try:
        assert reply(locker) == "ready"
        request("lock")
        wait_for(lambda: request("counts")["locked"])
        wait_for(lambda: not s.status()["views"])
        state = s.status()
        assert not state["zoom"]["held"] and state["zoom"]["extra_factor"] == state["zoom"]["extra_goal"] == 1
        assert not state["zoom"]["pan_held"] and not state["zoom"]["panning"]
        assert not state["cursor_owned"] and not state["keyboard_owned"]
        assert all(window["alpha"] == 1 for window in state["windows"])
        before = request("counts")["buttons"]
        s.button(0, PAN_BUTTON)
        assert request("counts")["buttons"] == before
        s.button(1, PAN_BUTTON)
        s.button(0, PAN_BUTTON)
        wait_for(lambda: request("counts")["buttons"] == before + 2)
        s.key(Z, 0)
        request("unlock")
        wait_for(lambda: not request("counts")["locked"])
        s.check("locking a panned overview clears camera and grip ownership, consumes the old release and passes a fresh native right-button cycle to the locker")
    finally:
        s.button(0, PAN_BUTTON)
        s.key(Z, 0)
        if locker.poll() is None:
            if request("counts")["locked"]:
                request("unlock")
            locker.terminate()
            locker.wait(timeout=3)

    opened(s, wait_for)
    s.key(Z, 1)
    inspection(s, wait_for, 1)
    s.move(center(tile(s, 11)))
    wheel(s, -3)
    inspection(s, wait_for, 1.15 ** 3)
    s.button(1, PAN_BUTTON)
    wait_for(lambda: s.status()["zoom"]["panning"])
    s.ctl("plugin", "unload", str(s.plugin))
    s.button(0, PAN_BUTTON)
    s.key(Z, 0)
    s.ctl("plugin", "load", str(s.plugin))
    s.ctl("reload")
    state = s.status()
    assert not state["live"] and not state["views"] and not state["zoom"]["held"]
    assert state["zoom"]["extra_factor"] == state["zoom"]["extra_goal"] == 1
    assert not state["zoom"]["pan_held"] and not state["zoom"]["panning"]
    assert state["resources"]["captures"]["bytes"] == state["resources"]["textures"]["bytes"] == 0
    opened(s, wait_for)
    s.key(Z, 1)
    inspection(s, wait_for, 1)
    s.move(center(tile(s, 11)))
    wheel(s, -1)
    inspection(s, wait_for, 1.15)
    s.button(1, PAN_BUTTON)
    wait_for(lambda: s.status()["zoom"]["panning"])
    s.button(0, PAN_BUTTON)
    s.key(Z, 0)
    released(s, wait_for)
    s.close()
    s.check("unloading a panned overview releases camera, cursor and resources; reloading begins without stale inspection, grip or callbacks")

    s.setup("dwindle", 1, 1)
    opened(s, wait_for, workspace=12)
    s.key(Z, 1)
    inspection(s, wait_for, 1)
    s.move(center(tile(s, 12)))
    wheel(s, -2)
    inspection(s, wait_for, 1.15 ** 2)
    s.button(1, PAN_BUTTON)
    wait_for(lambda: s.status()["zoom"]["panning"])
    try:
        s.ctl("keyword", "monitor", f"{s.names[1]},disable")
        s.await_outputs(2)
        released(s, wait_for)
        assert s.status()["zoom"]["extra_factor"] == s.status()["zoom"]["extra_goal"] == 1
        assert not s.status()["zoom"]["pan_held"] and not s.status()["zoom"]["panning"]
        s.button(0, PAN_BUTTON)
        s.key(Z, 0)
        s.check("removing the selected output clears magnification, the active pan grip and held zoom without stale target callbacks")
    finally:
        s.button(0, PAN_BUTTON)
        s.key(Z, 0)
        s.ctl("keyword", "monitor", f"{s.names[1]},960x600@60,-1000x-200,1.25,transform,1")
        s.await_outputs(3)
        s.close()
