"""Quick fit gestures preserve mouse control; magnified inspection returns locally."""

import json
import time

from resize import area
from zoom import F, Z, center, inspection, load_overview_fixture, relative_to, tile, wheel


def frame(s, monitor):
    value = s.ctl("dispatch", "hyprspace-test:zoom-cursor-frame", monitor)
    return json.loads(value) if value.startswith("{") else None


def box(value, workspace):
    return value["board"] if workspace is None else value["cells"][str(workspace)]


def normalized(value, workspace):
    preview = box(value, workspace)
    return tuple((value["cursor"][axis] - preview[axis]) / preview[size] for axis, size in (("x", "w"), ("y", "h")))


def stationary_cycles(s, wait_for, monitor, workspace, animated, count=12, screenshots=False):
    s.ctl("keyword", "animations:enabled", "false")
    preview = tile(s, workspace, monitor) if workspace is not None else next(
        p for p in s.status()["window_board"]["previews"] if p["window"] == s.status()["window_board"]["selected_window"])
    s.move((preview["x"] + preview["w"] * .02, preview["y"] + preview["h"] * .5))
    if workspace is not None:
        assert s.status()["target"]["workspace"] == workspace
    before = json.loads(s.ctl("dispatch", "hyprspace-test:zoom-cursor-sample", monitor))
    if screenshots:
        s.run("grim", "-s", "1", "-c", "-o", monitor, str(s.root / "cursor-cycle-before.png"))
    s.ctl("keyword", "animations:enabled", str(animated).lower())
    previous_frame = before["frame"]
    for index in range(count):
        s.key(Z, 1)
        if not animated:
            zoomed = wait_for(lambda: (value := frame(s, monitor)) and value["frame"] > previous_frame and
                             value["held"] and value["camera_settled"] and value)
            assert not zoomed["cursor_travel"]
            previous_frame = zoomed["frame"]
            assert zoomed["cursor"] == before["cursor"], (before, zoomed, index)
        if animated:
            time.sleep(.035)
        if index % 2 == 0:
            s.key(57, 1)  # Space without any pointer movement
        s.key(Z, 0)
        if index % 2 == 0:
            s.key(57, 0)
        if animated:
            time.sleep(.035)
    after = wait_for(lambda: (value := frame(s, monitor)) and value["frame"] > before["frame"] and
                     not value["held"] and not value["returning"] and value)
    if screenshots:
        s.run("grim", "-s", "1", "-c", "-o", monitor, str(s.root / "cursor-cycle-after.png"))
    report = {"before": before, "after": after, "animated": animated, "cycles": count}
    (s.root / f"cursor-cycle-{monitor}-{workspace}-{animated}.json").write_text(json.dumps(report, indent=2))
    for axis in ("x", "y"):
        assert abs(after["cursor"][axis] - before["cursor"][axis]) < .1, report


def quick_motion_cycles(s, wait_for, monitor, workspace, animated, screenshots=False):
    s.ctl("keyword", "animations:enabled", "false")
    preview = tile(s, workspace, monitor) if workspace is not None else next(
        p for p in s.status()["window_board"]["previews"] if p["window"] == s.status()["window_board"]["selected_window"])
    start = preview["x"] + preview["w"] * .2, preview["y"] + preview["h"] * .5

    def sample():
        return json.loads(s.ctl("dispatch", "hyprspace-test:zoom-cursor-sample", monitor))

    def capture(stage):
        if screenshots:
            s.run("grim", "-s", "1", "-c", "-o", monitor, str(s.root / f"quick-zoom-{stage}.png"))

    # Measure native device motion independently of held zoom on each output.
    s.move(start)
    baseline = sample()
    s.motion(12, 5)
    moved = sample()
    s.motion(9, -3)
    continued = sample()
    initial_delta = {axis: moved["cursor"][axis] - baseline["cursor"][axis] for axis in ("x", "y")}
    final_delta = {axis: continued["cursor"][axis] - baseline["cursor"][axis] for axis in ("x", "y")}
    s.ctl("keyword", "animations:enabled", str(animated).lower())
    reports = []
    for delay in (0, .035, 0, .035):
        s.move(start)
        before = sample()
        capture("before")
        s.key(Z, 1)
        pressed = sample()
        assert pressed["held"] and not pressed["cursor_travel"], pressed
        assert pressed["cursor"] == before["cursor"], (before, pressed)
        if delay:
            time.sleep(delay)
        s.motion(12, 5)
        moved = sample()
        assert not moved["cursor_travel"], moved
        for axis in ("x", "y"):
            assert abs(moved["cursor"][axis] - before["cursor"][axis] - initial_delta[axis]) < .1, (before, moved, initial_delta)
        capture("held")
        s.key(Z, 0)
        s.motion(9, -3)
        released = sample()
        assert not released["held"] and not released["cursor_return"], released
        expected = {axis: before["cursor"][axis] + final_delta[axis] for axis in ("x", "y")}
        values = []
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            value = frame(s, monitor)
            if value and value["frame"] > released["frame"] and not value["held"]:
                assert not value["cursor_return"], value
                for axis in ("x", "y"):
                    assert abs(value["cursor"][axis] - expected[axis]) < .1, (before, value, expected)
                values.append(value)
                if not value["returning"]:
                    break
            time.sleep(.015)
        else:
            raise AssertionError(("quick zoom did not return", s.status(), values))
        capture("after")
        reports.append({"before": before, "pressed": pressed, "moved": moved, "released": released,
                        "frames": values, "expected": expected, "delay": delay})
    (s.root / f"quick-zoom-{monitor}-{workspace}-{animated}.json").write_text(json.dumps(reports, indent=2))


def release(s, wait_for, monitor, workspace, animated=True, motion=None, device_release=False, motion_count=1, screenshots=False):
    def capture(stage):
        if screenshots:
            s.run("grim", "-s", "1", "-c", "-o", monitor, str(s.root / f"cursor-return-motion-{stage}.png"))

    capture("before")
    if animated and not device_release:
        response = s.ctl("dispatch", "hyprspace-test:zoom-cursor-release", monitor)
        assert response.startswith("{"), response
        before = json.loads(response)
    else:
        before = json.loads(s.ctl("dispatch", "hyprspace-test:zoom-cursor-sample", monitor))
    s.key(Z, 0)
    point = normalized(before, workspace)
    previous_frame = before["frame"]
    if motion is not None:
        for index in range(motion_count):
            s.motion(*motion)
            moved = json.loads(s.ctl("dispatch", "hyprspace-test:zoom-cursor-sample", monitor))
            assert not moved["held"] and moved["cursor_return"] and moved["returning"], moved
            if index + 1 < motion_count:
                time.sleep(.02)
        # Animation values can tick on another output before this output has
        # rendered. Compare the cursor with geometry from the same real frame.
        first = wait_for(lambda: (value := frame(s, monitor)) and value["frame"] > moved["frame"] and value)
        assert first["cursor_return"] and first["returning"], first
        point = normalized(first, workspace)
        previous_frame = first["frame"]
    capture("during")
    samples = []
    deadline = time.monotonic() + 6
    while time.monotonic() < deadline:
        value = frame(s, monitor)
        if value and value["frame"] > previous_frame and not value["held"]:
            preview = box(value, workspace)
            for fraction, axis, size in zip(point, ("x", "y"), ("w", "h")):
                expected = preview[axis] + fraction * preview[size]
                assert abs(value["cursor"][axis] - expected) < .1, (before, value, axis, expected)
            samples.append(value)
            if not value["returning"]:
                break
        time.sleep(.015)
    else:
        (s.root / "cursor-return-timeout.json").write_text(json.dumps({"before": before, "samples": samples, "frame": frame(s, monitor),
                                                                    "status": s.status(), "cursor": s.data("cursorpos")}, indent=2))
        raise AssertionError("cursor return did not finish")
    assert samples
    if animated:
        assert any(value["returning"] for value in samples), samples
        assert any(abs(value["cursor"][axis] - before["cursor"][axis]) > 10 for value in samples for axis in ("x", "y")), samples
    capture("after")
    (s.root / f"cursor-return-{monitor}-{workspace}-{len(s.checks)}.json").write_text(json.dumps({"before": before, "frames": samples}, indent=2))
    return before, samples[-1]


def run(s, wait_for):
    fixture = load_overview_fixture(s, wait_for)
    try:
        s.setup("dwindle", 0, 0)
        native = s.geometry()
        for index, monitor in enumerate(s.names):
            s.ctl("keyword", "workspace", f"{51 + index * 10},monitor:{monitor},persistent:true")
        s.ctl("dispatch", "hyprspace:overview", "on")
        wait_for(lambda: len(s.status()["views"]) == 3)
        s.ctl("keyword", "animation", "windowsMove,1,8,default")

        for index, monitor in enumerate(s.names):
            for workspace in (51 + index * 10, 11 + index):
                stationary_cycles(s, wait_for, monitor, workspace, False, screenshots=index == 0 and workspace == 51)
                stationary_cycles(s, wait_for, monitor, workspace, True)
            s.check(f"repeated Z and Z+Space cycles preserve a stationary cursor on either workspace on {monitor}")
            for workspace in (51 + index * 10, 11 + index):
                quick_motion_cycles(s, wait_for, monitor, workspace, False)
                quick_motion_cycles(s, wait_for, monitor, workspace, True, screenshots=index == 0 and workspace == 51)
            s.check(f"quick Z, mouse motion and release retain native cursor movement on {monitor}")

        s.ctl("keyword", "animations:enabled", "false")
        for index, monitor in enumerate(s.names):
            s.ctl("keyword", "workspace", f"{52 + index * 10},monitor:{monitor},persistent:true")

        for index, monitor in enumerate(s.names):
            workspace = 11 + index
            s.ctl("keyword", "animations:enabled", "false")
            s.move(center(tile(s, workspace, monitor)))
            s.key(Z, 1)
            inspection(s, wait_for, 1)
            s.move(center(tile(s, workspace, monitor)))
            wheel(s, -32)
            inspection(s, wait_for, 4)
            if not s.status()["zoom"]["cursor_follow_enabled"]:
                s.key(F, 1)
                s.key(F, 0)
            work = area(next(m for m in s.data("monitors") if m["name"] == monitor))
            relative_to(s, (work["x"] + work["w"] * (.8 if index == 1 else .2), work["y"] + work["h"] * .2))
            wait_for(lambda: not s.status()["zoom"]["cursor_follow_settling"])
            s.ctl("keyword", "animations:enabled", "true")
            release(s, wait_for, monitor, workspace)
            s.check(f"Z release carries the cursor with its content on scaled/transformed {monitor}")

        monitor = s.names[0]
        s.ctl("keyword", "animations:enabled", "false")
        s.move(center(tile(s, 11, monitor)))
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        s.move(center(tile(s, 11, monitor)))
        s.ctl("keyword", "animations:enabled", "true")
        wheel(s, -32)
        relative_to(s, (s.data("cursorpos")["x"] + 35, s.data("cursorpos")["y"] - 30))
        wait_for(lambda: s.status()["zoom"]["extra_factor"] > 1.2 and s.status()["zoom"]["inspection_transitioning"])
        release(s, wait_for, monitor, 11)
        s.check("releasing Z during wheel magnification preserves the displayed point through zoom-out")

        s.ctl("keyword", "animations:enabled", "false")
        s.move(center(tile(s, 11, monitor)))
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        s.move(center(tile(s, 11, monitor)))
        wheel(s, -32)
        inspection(s, wait_for, 4)
        s.ctl("keyword", "animations:enabled", "true")
        release(s, wait_for, monitor, 11, motion=(.25, -.25), screenshots=True)
        s.check("subpixel mouse movement after release retains the local cursor position through zoom-out")

        output = area(next(m for m in s.data("monitors") if m["name"] == monitor))
        for corner in ((.02, .98), (.98, .98), (.98, .02), (.02, .02), (.8, .8)):
            s.ctl("keyword", "animations:enabled", "false")
            s.move(center(tile(s, 11, monitor)))
            s.key(Z, 1)
            inspection(s, wait_for, 1)
            s.move(center(tile(s, 11, monitor)))
            wheel(s, -32)
            inspection(s, wait_for, 4)
            s.ctl("keyword", "animations:enabled", "true")
            relative_to(s, (output["x"] + output["w"] * corner[0], output["y"] + output["h"] * corner[1]))
            release(s, wait_for, monitor, 11, motion=(.25, -.25), device_release=True)
        s.check("quick corner motions followed by Z release and residual mouse motion consistently complete the return")

        s.ctl("keyword", "animations:enabled", "false")
        s.move(center(tile(s, 11, monitor)))
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        s.move(center(tile(s, 11, monitor)))
        wheel(s, -32)
        inspection(s, wait_for, 4)
        s.ctl("keyword", "animations:enabled", "true")
        release(s, wait_for, monitor, 11, motion=(5, 3), device_release=True, motion_count=6)
        s.check("continuous mouse movement steers the local point while the cursor continues returning")

        s.ctl("keyword", "animations:enabled", "false")
        s.move(center(tile(s, 11, monitor)))
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        s.move(center(tile(s, 11, monitor)))
        wheel(s, -32)
        inspection(s, wait_for, 4)
        s.ctl("keyword", "animations:enabled", "true")
        s.key(Z, 0)
        wait_for(lambda: (value := frame(s, monitor)) and value["returning"] and value["cursor_return"])
        s.move((output["x"] + 120, output["y"] + 100))
        pointer = s.data("cursorpos")
        wait_for(lambda: (value := frame(s, monitor)) and not value["returning"])
        assert s.data("cursorpos") == pointer
        s.check("an explicit cursor warp still takes control without being pulled back")

        s.ctl("keyword", "animations:enabled", "false")
        s.move(center(tile(s, 11, monitor)))
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        s.move(center(tile(s, 11, monitor)))
        wheel(s, -32)
        inspection(s, wait_for, 4)
        relative_to(s, (s.data("cursorpos")["x"] + 30, s.data("cursorpos")["y"] - 25))
        wait_for(lambda: not s.status()["zoom"]["cursor_follow_settling"])
        release(s, wait_for, monitor, 11, animated=False)
        s.check("disabled animations remap the cursor directly to the corresponding grid point")

        s.move(center(tile(s, 11, monitor)))
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        s.move(center(tile(s, 11, monitor)))
        wheel(s, -32)
        inspection(s, wait_for, 4)
        s.move(center(tile(s, 12, s.names[1])))
        pointer = s.data("cursorpos")
        s.ctl("keyword", "animations:enabled", "true")
        s.key(Z, 0)
        wait_for(lambda: not s.status()["zoom"]["returning"])
        assert s.data("cursorpos") == pointer
        s.check("releasing zoom with the cursor on another output preserves that output's cursor position")

        s.ctl("keyword", "animations:enabled", "false")
        namespace = "waybar-cursor-return"
        panel = s.spawn(["python3", str(s.artifact("layer.py")), str(s.root / "cursor-return-panel"), namespace, "panel", "0,0"])
        try:
            wait_for(lambda: s.layer(namespace))
            s.move(center(tile(s, 11, monitor)))
            s.key(Z, 1)
            inspection(s, wait_for, 1)
            s.move(center(tile(s, 11, monitor)))
            wheel(s, -32)
            inspection(s, wait_for, 4)
            output = next(m for m in s.data("monitors") if m["name"] == monitor)
            relative_to(s, (output["x"] + output["width"] / output["scale"] / 2, output["y"] + 10))
            wait_for(lambda: not s.status()["zoom"]["cursor_follow_settling"])
            assert s.status()["cursor_owned"]
            s.ctl("keyword", "animations:enabled", "true")
            release(s, wait_for, monitor, 11, motion=(.25, .25))
            s.check("releasing Z over Waybar with residual motion returns the cursor with the zoomed content")
        finally:
            panel.terminate()
            panel.wait(timeout=5)
            wait_for(lambda: not s.layer(namespace))

        s.ctl("keyword", "animations:enabled", "false")
        s.move(center(tile(s, 11, monitor)))
        s.ctl("dispatch", "hyprspace:windowview", "app")
        wait_for(lambda: s.status()["window_board"]["active"])
        stationary_cycles(s, wait_for, monitor, None, False)
        stationary_cycles(s, wait_for, monitor, None, True)
        s.check("repeated inspection cycles preserve a stationary cursor in the window board")
        quick_motion_cycles(s, wait_for, monitor, None, False)
        quick_motion_cycles(s, wait_for, monitor, None, True)
        s.check("quick window fit, mouse motion and release leave the cursor under user control")
        s.ctl("keyword", "animations:enabled", "false")
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        board = s.status()["window_board"]
        s.move(center(next(p for p in board["previews"] if p["window"] == board["selected_window"])))
        wheel(s, -32)
        inspection(s, wait_for, 4)
        relative_to(s, (s.data("cursorpos")["x"] + 35, s.data("cursorpos")["y"] - 25))
        wait_for(lambda: not s.status()["zoom"]["cursor_follow_settling"])
        s.ctl("keyword", "animations:enabled", "true")
        release(s, wait_for, monitor, None, motion=(5, 3), device_release=True, motion_count=6)
        s.check("window inspection returns the cursor to its card while allowing continued mouse movement")

        s.ctl("keyword", "animations:enabled", "false")
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        board = s.status()["window_board"]
        s.move(center(next(p for p in board["previews"] if p["window"] == board["selected_window"])))
        s.ctl("keyword", "animations:enabled", "true")
        wheel(s, -32)
        wait_for(lambda: s.status()["zoom"]["extra_factor"] > 1.2 and s.status()["zoom"]["inspection_transitioning"])
        release(s, wait_for, monitor, None, motion=(.25, -.25), device_release=True)
        s.check("quick window zoom release with residual motion returns without a cursor jump")
        assert s.geometry() == native
    finally:
        s.key(Z, 0)
        s.close()
        s.ctl("keyword", "animations:enabled", "false")
        s.ctl("plugin", "unload", str(fixture))
