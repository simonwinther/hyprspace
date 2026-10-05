"""Real-motion thresholds, pointer capture and released resize previews."""

import math
import os
from pathlib import Path
import signal
import time

from resize import area, reset_float


def geometry(window):
    return window["at"] + window["size"]


def floating(s):
    s.setup("dwindle", 0, 2)
    address = s.windows()["hs-A"]["address"]
    work = area(next(m for m in s.data("monitors") if m["name"] == s.names[0]))
    return address, work


def thresholds(s, wait_for):
    address, work = floating(s)
    # One workspace, no labels and this padding produce an exact 3/4 scale.
    # Integral preview steps can therefore exercise the exact native boundary.
    s.ctl("keyword", "plugin:hyprspace:overview:workspace_labels", "false")
    s.ctl("keyword", "plugin:hyprspace:overview:padding", "75")
    s.ctl("keyword", "general:snap:enabled", "false")
    try:
        for button in (272, 273):
            corners = ((False, False),) if button == 272 else (
                (False, False), (True, False), (False, True), (True, True)
            )
            for left, top in corners:
                for axis in (0, 1):
                    for sign in (-1, 1):
                        for threshold, step, commit in (
                            (0, 0, False), (0, 3, True),
                            (12, 6, False), (12, 9, False), (12, 12, True),
                            (120, 6, False),
                        ):
                            s.ctl("keyword", "binds:drag_threshold", str(threshold))
                            reset_float(s, address, work)
                            original = s.windows()["hs-A"]
                            preview = s.preview("hs-A")
                            scales = [preview["w"] / original["size"][0], preview["h"] / original["size"][1]]
                            assert all(math.isclose(scale, .75, abs_tol=1e-9) for scale in scales), scales
                            s.move(s.preview_point("hs-A", .2 if left else .8, .2 if top else .8))
                            pointer = s.data("cursorpos")
                            point = [pointer["x"], pointer["y"]]
                            point[axis] += sign * step
                            expected = geometry(original)
                            delta = sign * step / scales[axis]
                            if commit:
                                if button == 272:
                                    expected[axis] += delta
                                else:
                                    grabbed_start = left if axis == 0 else top
                                    expected[axis + 2] += -delta if grabbed_start else delta
                                    if grabbed_start:
                                        expected[axis] += delta
                            s.key(125, 1)
                            pressed = False
                            try:
                                s.button(1, button)
                                pressed = True
                                shape = "grabbing" if button == 272 else (
                                    "nw-resize" if left and top else "ne-resize" if top else "sw-resize" if left else "se-resize"
                                )
                                assert s.status()["cursor_shape"] == shape
                                s.move(point)
                                assert s.status()["drag"]["moved"] == commit
                                assert geometry(s.windows()["hs-A"]) == geometry(original)
                                s.button(0, button)
                                pressed = False
                            finally:
                                if pressed:
                                    s.button(0, button)
                                s.key(125, 0)
                            wait_for(lambda: not s.status().get("pending_resize", False))
                            assert s.status()["cursor_shape"] == "default"
                            actual = geometry(s.windows()["hs-A"])
                            assert all(abs(a - e) <= 1e-6 for a, e in zip(actual, expected)), {
                                "button": button, "left": left, "top": top,
                                "axis": axis, "threshold": threshold,
                                "preview_delta": sign * step, "mapped_delta": delta,
                                "expected": expected, "actual": actual,
                            }
                            assert not s.status()["dragging"] and s.status()["layout_targets_unique"]
                s.check(f"{'move' if button == 272 else 'resize'} corner {left}/{top}: exact zero, below, at and above thresholds in all directions")
    finally:
        s.close()
        s.ctl("keyword", "binds:drag_threshold", "0")
        s.ctl("keyword", "plugin:hyprspace:overview:workspace_labels", "true")
        s.ctl("keyword", "plugin:hyprspace:overview:padding", "56")


def panel_capture(s, wait_for):
    address, work = floating(s)
    entry = s.root / "gesture-panel"
    panel = s.spawn(["python3", str(s.artifact("layer.py")), str(entry), "waybar", "bottom"])
    try:
        wait_for(lambda: s.layer("waybar"))
        for button in (272, 273):
            reset_float(s, address, work)
            s.move(s.preview_point("hs-A", .8, .8))
            s.key(125, 1)
            pressed = False
            try:
                s.button(1, button)
                pressed = True
                assert s.status()["dragging"]
                layer = s.layer("waybar")
                s.move((layer["x"] + 70, layer["y"] + 50))
                before = geometry(s.windows()["hs-A"])
                for _ in range(12):
                    state = s.status()
                    assert state["dragging"] and state["cursor_owned"], state
                    assert state["cursor_shape"] == ("grabbing" if button == 272 else "se-resize"), state
                    time.sleep(.025)
                s.scroll()
                assert geometry(s.windows()["hs-A"]) == before
                s.button(0, button)
                pressed = False
            finally:
                if pressed:
                    s.button(0, button)
                s.key(125, 0)
            wait_for(lambda: not s.status().get("pending_resize", False))
            wait_for(lambda: not s.status()["cursor_owned"])
            assert not Path(str(entry) + ".click").exists()
            s.check(f"{'move' if button == 272 else 'resize'} captures a stationary panel pointer and scroll, then restores layer routing")
    finally:
        s.close()
        panel.terminate()
        panel.wait(timeout=3)


def pending(s, wait_for):
    from PIL import Image

    def green_bounds(path):
        image = Image.open(path).convert("RGB")
        pixels = image.load()
        marked = [
            (x, y)
            for y in range(image.height)
            for x in range(image.width)
            if pixels[x, y][1] > 150
            and pixels[x, y][1] > max(pixels[x, y][0], pixels[x, y][2]) + 90
        ]
        assert len(marked) > 100, ("pending resize content was not rendered", path)
        xs, ys = zip(*marked)
        return min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1

    previous_marker = s.env.get("HS_CONTENT_MARKER")
    s.env["HS_CONTENT_MARKER"] = "hs-A"
    try:
        address, work = floating(s)
    finally:
        if previous_marker is None:
            s.env.pop("HS_CONTENT_MARKER")
        else:
            s.env["HS_CONTENT_MARKER"] = previous_marker
    # Only the native resize clock is slowed; the private output still renders
    # ordinary frames, allowing the released preview to be observed reliably.
    s.ctl("dispatch", "hyprspace-test:resize-clock", "hold")
    try:
        for cancel in (False, True):
            reset_float(s, address, work)
            s.move(s.preview_point("hs-A", .8, .8))
            start = s.data("cursorpos")
            before = s.windows()["hs-A"]["size"]
            paths = [s.root / f"pending-resize-{stage}.png" for stage in ("before", "held", "released")]
            if not cancel:
                s.run("grim", "-s", "1", "-o", s.names[0], str(paths[0]))
            s.key(125, 1)
            pressed = None
            try:
                s.button(1, 273)
                pressed = 273
                s.move((start["x"] + 18, start["y"] + 12))
                if not cancel:
                    s.run("grim", "-s", "1", "-o", s.names[0], str(paths[1]))
                s.button(0, 273)
                pressed = None
                if not cancel:
                    s.run("grim", "-s", "1", "-o", s.names[0], str(paths[2]))
                state = s.status()
                assert not state["dragging"] and state["pending_resize"], state
                assert s.windows()["hs-A"]["size"] == before
                # A second gesture cannot overwrite the released native commit.
                s.button(1, 272)
                pressed = 272
                assert not s.status()["dragging"]
                s.button(0, 272)
                pressed = None
                if cancel:
                    s.ctl("dispatch", "hyprspace:overview", "off")
            finally:
                if pressed is not None:
                    s.button(0, pressed)
                s.key(125, 0)
            wait_for(lambda: not s.status()["pending_resize"])
            if cancel:
                assert s.windows()["hs-A"]["size"] == before
            else:
                assert s.windows()["hs-A"]["size"] != before
                assert s.status()["live"] and s.status()["layout_targets_unique"]
                old, held, released = map(green_bounds, paths)
                assert held[2] >= old[2] + 16 and held[3] >= old[3] + 10, (old, held)
                assert all(abs(a - b) <= 1 for a, b in zip(held, released)), (held, released, paths)
            s.check(f"released resize retains its preview without pressed-button capture and {'cancels on dismissal' if cancel else 'commits after immediate modifier release'}")
    finally:
        s.close()
        s.ctl("dispatch", "hyprspace-test:resize-clock", "reset")

    address, _ = floating(s)
    panel = s.spawn(["python3", str(s.artifact("layer.py")), str(s.root / "pending-panel"), "waybar", "bottom"])
    try:
        wait_for(lambda: s.layer("waybar"))
        work = area(next(m for m in s.data("monitors") if m["name"] == s.names[0]))
        s.ctl("keyword", "binds:drag_threshold", "12")
        s.ctl("dispatch", "hyprspace-test:resize-clock", "hold")
        # Native resize timing is shared across gestures. Repeating after idle
        # exercises raw motion that native code would process immediately.
        for cycle in range(3):
            reset_float(s, address, work)
            time.sleep(.3)
            s.move(s.preview_point("hs-A", .8, .8))
            start = s.data("cursorpos")
            before = s.windows()["hs-A"]
            preview = s.preview("hs-A")
            expected = [round(before["size"][0] + 18 * before["size"][0] / preview["w"]),
                        round(before["size"][1] + 12 * before["size"][1] / preview["h"])]
            s.key(125, 1)
            pressed = False
            try:
                s.button(1, 273)
                pressed = True
                s.move((start["x"] + 18, start["y"] + 12))
                assert s.status()["drag"]["moved"]
                s.button(0, 273)
                pressed = False
            finally:
                if pressed:
                    s.button(0, 273)
                s.key(125, 0)
            assert s.status()["pending_resize"]
            layer = s.layer("waybar")
            # A raw warp avoids the dummy button frame used by Suite.move;
            # only pointer motion can affect this released native gesture.
            s.ctl("dispatch", "movecursor", f"{layer['x'] + 70} {layer['y'] + 50}")
            time.sleep(.04)
            state = s.status()
            assert state["pending_resize"] and not state["cursor_owned"], state
            assert geometry(s.windows()["hs-A"]) == geometry(before), (cycle, before, s.windows()["hs-A"])
            wait_for(lambda: not s.status()["pending_resize"])
            assert s.windows()["hs-A"]["size"] == expected, (cycle, expected, s.windows()["hs-A"])
            assert s.status()["layout_targets_unique"]
        s.check("released resizes preserve their native geometry across raw panel hover and idle between gestures")
    finally:
        s.ctl("keyword", "binds:drag_threshold", "0")
        s.ctl("dispatch", "hyprspace-test:resize-clock", "reset")
        s.close()
        panel.terminate()
        panel.wait(timeout=3)

    for failure in ("window loss", "output removal"):
        address, work = floating(s)
        reset_float(s, address, work)
        s.move(s.preview_point("hs-A", .8, .8))
        start = s.data("cursorpos")
        original = s.windows()["hs-A"]
        removed = next(m for m in s.data("monitors") if m["name"] == s.names[2])
        s.ctl("dispatch", "hyprspace-test:resize-clock", "hold")
        try:
            s.key(125, 1)
            pressed = False
            try:
                s.button(1, 273)
                pressed = True
                s.move((start["x"] + 18, start["y"] + 12))
                s.button(0, 273)
                pressed = False
                assert s.status()["pending_resize"]
                if failure == "window loss":
                    os.kill(original["pid"], signal.SIGTERM)
                else:
                    s.ctl("keyword", "monitor", f"{s.names[2]},disable")
                    s.await_outputs(2)
                wait_for(lambda: not s.status()["pending_resize"])
            finally:
                if pressed:
                    s.button(0, 273)
                s.key(125, 0)
            time.sleep(.25)  # No late timer may resurrect the cancelled preview.
            state = s.status()
            assert state["live"] and not state["dragging"] and not state["pending_resize"]
            assert state["layout_targets_unique"]
            if failure == "window loss":
                assert "hs-A" not in s.windows()
            else:
                assert s.windows()["hs-A"]["size"] == original["size"]
            s.check(f"{failure} cancels a released resize and prevents stale timer completion")
        finally:
            s.ctl("dispatch", "hyprspace-test:resize-clock", "reset")
            if failure == "output removal":
                s.ctl("keyword", "monitor", f"{s.names[2]},{removed['width']}x{removed['height']}@60,{removed['x']}x{removed['y']},{removed['scale']},transform,{removed['transform']}")
                s.await_outputs(3)
            s.close()


def run(s, wait_for):
    fixture = s.artifact("test-overview.so")
    s.ctl("plugin", "load", str(fixture))
    time.sleep(.15)  # Plugin loading schedules a native configuration reload.
    try:
        s.ctl("dispatch", "hyprspace-test:cursor")
        s.check("cursor shape changes and release preserve a newer native override")
        thresholds(s, wait_for)
        panel_capture(s, wait_for)
        pending(s, wait_for)
    finally:
        s.close()
        s.ctl("plugin", "unload", str(fixture))
