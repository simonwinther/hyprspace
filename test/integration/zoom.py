"""Hold-to-zoom input, displayed geometry and transition regressions."""

import json
import subprocess
import time

from protocol import reply
from resize import area, inside, window_box


Z, X, TAB, SHIFT, SUPER = 44, 45, 15, 42, 125


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


def opened(s, wait_for, workspace=11):
    s.ctl("dispatch", "hyprspace:overview", "on")
    wait_for(lambda: len(s.status()["views"]) == 3)
    s.move(center(tile(s, workspace)))
    # Opening tiles can overlap, so keyboard selection establishes the measured
    # destination independently of a moving pointer hit during the transition.
    s.key(102, 1)  # Home: each fixture opens on its output's first workspace.
    s.key(102, 0)
    assert s.status()["target"]["workspace"] == workspace


def lifecycle(s, wait_for):
    defaults = ("zoom_key", "padding", "workspace_labels", "include_special")
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
        s.key(Z, 0)
        released(s, wait_for)
        s.check("hold zoom is a geometry no-op for an output with one workspace")

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
        s.key(Z, 0)
        assert s.status()["zoom"]["held"]
        other_key(Z, 0)
        released(s, wait_for)
        other_key(Z, 1)
        secondary.terminate()
        secondary.wait(timeout=3)
        secondary = None
        released(s, wait_for)
        s.check("zoom captures are per keyboard and device removal clears a held zoom")

        s.key(Z, 1)
        entry = s.root / "zoom-foreground"
        layer = s.spawn(["python3", str(s.artifact("layer.py")), str(entry)])
        wait_for(lambda: s.layer("hs-foreground"))
        wait_for(lambda: not s.status()["keyboard_owned"])
        released(s, wait_for)
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
        s.check("foreground keyboard handoff cancels zoom and restoration requires a fresh press")
        s.close()

        reload_and_removal(s, wait_for)
        gestures(s, wait_for)
        transitions(s, wait_for)
        scrolling(s, wait_for)
        transformed(s, wait_for)
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
    s.ctl("reload")
    released(s, wait_for)
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


def gestures(s, wait_for):
    s.setup("dwindle", 0, 1)
    address = s.windows()["hs-A"]["address"]
    work = area(next(m for m in s.data("monitors") if m["name"] == s.names[0]))
    s.ctl("dispatch", "setfloating", "address:" + address)
    s.ctl("dispatch", "resizewindowpixel", f"exact 240 160,address:{address}")
    s.ctl("dispatch", "movewindowpixel", f"exact {work['x'] + 160:.0f} {work['y'] + 160:.0f},address:{address}")
    opened(s, wait_for)
    for button in (272, 273):
        s.key(Z, 1)
        wait_for(lambda: s.status()["zoom"]["held"])
        s.move(s.preview_point("hs-A", 0.8, 0.8))
        original = s.geometry()
        s.key(SUPER, 1)
        s.button(1, button)
        assert s.status()["dragging"]
        s.key(Z, 0)
        released(s, wait_for)
        assert s.status()["dragging"] and s.geometry() == original
        s.move(s.preview_point("hs-A", 0.8, 0.8))
        s.run("wtype", "-k", "Escape")
        s.button(0, button)
        s.key(SUPER, 0)
        assert not s.status()["dragging"] and s.geometry() == original
    s.check("zoom release returns the grid during move and resize gestures without committing native geometry")

    s.key(Z, 1)
    wait_for(lambda: s.status()["zoom"]["held"])
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


def scrolling(s, wait_for):
    s.setup("scrolling", 0, 0)
    opened(s, wait_for)
    s.key(Z, 1)
    wait_for(lambda: s.status()["zoom"]["held"])
    s.move(center(tile(s, 11)))
    before = s.geometry()
    s.scroll()
    wait_for(lambda: s.geometry() != before)
    assert s.status()["zoom"]["held"] and s.status()["zoom"]["workspace"] == 11
    s.key(Z, 0)
    released(s, wait_for)
    s.check("scrolling-layout previews continue to pan while workspace zoom is held")
    s.close()


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
        enlarged = tile(s, 12)
        assert enlarged["w"] > baseline[(s.names[1], 12)]["w"] * 1.5
        assert inside(enlarged, work)
        assert abs(enlarged["w"] / enlarged["h"] - work["w"] / work["h"]) < 0.001
        assert same_geometry(
            {key: value for key, value in baseline.items() if key[0] != s.names[1]},
            {key: value for key, value in tiles(s).items() if key[0] != s.names[1]},
        )
        s.move(s.preview_point("hs-A", 0.8, 0.8))
        selected = s.status()["target"]
        assert selected["window"] == s.windows()["hs-A"]["address"]
        expected = s.point(s.windows()["hs-A"], 0.8, 0.8)
        assert abs(selected["x"] - expected[0]) < 2.5 and abs(selected["y"] - expected[1]) < 2.5, (selected, expected)
        s.key(TAB, 1)
        s.key(TAB, 0)
        assert s.status()["zoom"]["workspace"] == 31 and s.status()["zoom"]["monitor"] == s.names[1]
        assert abs(tile(s, 31)["w"] - enlarged["w"]) < 0.1
        s.key(Z, 0)
        released(s, wait_for)
        assert same_geometry(baseline, tiles(s))
        s.check("multiple workspaces zoom and hit-test correctly on a rotated output at fractional scale")
    finally:
        s.key(Z, 0)
        s.close()
        for workspace in range(31, 35):
            s.ctl("keyword", "workspace", f"{workspace},persistent:false")
