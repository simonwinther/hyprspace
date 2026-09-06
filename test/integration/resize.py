"""Workspace bounds for provisional and committed overview resizes."""

from pathlib import Path
import time


def area(monitor):
    width, height = monitor["width"], monitor["height"]
    if monitor["transform"] % 2:
        width, height = height, width
    left, top, right, bottom = monitor["reserved"]
    return {
        "x": monitor["x"] + left,
        "y": monitor["y"] + top,
        "w": width / monitor["scale"] - left - right,
        "h": height / monitor["scale"] - top - bottom,
    }


def inside(box, bounds, tolerance=1):
    return (
        box["x"] >= bounds["x"] - tolerance
        and box["y"] >= bounds["y"] - tolerance
        and box["x"] + box["w"] <= bounds["x"] + bounds["w"] + tolerance
        and box["y"] + box["h"] <= bounds["y"] + bounds["h"] + tolerance
    )


def window_box(window):
    return dict(zip(("x", "y", "w", "h"), window["at"] + window["size"]))


def reset_float(s, address, bounds, size=(240, 160)):
    s.close()
    s.ctl("dispatch", "setfloating", "address:" + address)
    s.ctl(
        "dispatch", "resizewindowpixel", f"exact {size[0]} {size[1]},address:{address}"
    )
    x = bounds["x"] + (bounds["w"] - size[0]) / 2
    y = bounds["y"] + (bounds["h"] - size[1]) / 2
    s.ctl("dispatch", "movewindowpixel", f"exact {x:.0f} {y:.0f},address:{address}")
    s.ctl("dispatch", "hyprspace:overview", "on")
    time.sleep(0.15)


def resize(s, destination, left=False, top=False, cancel=False, bounded=True):
    before = s.geometry()
    preview = s.preview("hs-A")
    tile = next(
        t
        for v in s.status()["views"]
        for t in v["tiles"]
        if t["workspace"] == preview["workspace"] and t["window"] == "0x0"
    )
    x, y = max(preview["x"], tile["x"]), max(preview["y"], tile["y"])
    width = min(preview["x"] + preview["w"], tile["x"] + tile["w"]) - x
    height = min(preview["y"] + preview["h"], tile["y"] + tile["h"]) - y
    # An oversized window can only be picked up through its visible portion.
    s.move((x + width * (0.2 if left else 0.8), y + height * (0.2 if top else 0.8)))
    s.key(125, 1)
    try:
        s.button(1, 273)
        assert s.status()["dragging"]
        s.move(destination)
        state = s.status()
        assert state["drag"]["resize"]
        if bounded:
            assert inside(state["drag"]["box"], state["drag"]["clip"]), state["drag"]
        assert s.geometry() == before, "resize changed native geometry before release"
        if cancel:
            s.run("wtype", "-k", "Escape")
        s.button(0, 273)
    finally:
        s.button(0, 273)
        s.key(125, 0)
    time.sleep(0.12)
    assert s.status()["live"] and not s.status()["dragging"]
    assert s.status()["modifiers"] == 0 and s.status()["layout_targets_unique"]
    if cancel:
        assert s.geometry() == before
    return before


def bounds(s, wait_for):
    # Extra reserved space exercises all four panel edges without opening panels
    # on the user's compositor. It also makes workspace and output bounds differ.
    for name in s.names:
        s.ctl("keyword", "monitor", f"{name},addreserved,36,14,12,18")
    try:
        for layout in ("dwindle", "scrolling", "master"):
            for index, name in enumerate(s.names):
                s.setup(layout, index, (index + 1) % 3)
                monitor = next(m for m in s.data("monitors") if m["name"] == name)
                work = area(monitor)
                address = s.windows()["hs-A"]["address"]
                for corner in range(4):
                    left, top = corner in (0, 3), corner in (0, 1)
                    reset_float(s, address, work)
                    original = window_box(s.windows()["hs-A"])
                    end = (
                        work["x"] + (1 if left else work["w"] - 1),
                        work["y"] + (1 if top else work["h"] - 1),
                    )
                    resize(s, end, left, top)
                    actual = s.windows()["hs-A"]
                    box = window_box(actual)
                    assert inside(box, work), (layout, index, corner, box, work)
                    assert actual["workspace"]["id"] == 11 + index
                    assert actual["monitor"] == monitor["id"]
                    assert box["w"] > original["w"] and box["h"] > original["h"]
                    for start, extent, flipped in (("x", "w", left), ("y", "h", top)):
                        assert (
                            abs(
                                box[start]
                                + (box[extent] if flipped else 0)
                                - original[start]
                                - (original[extent] if flipped else 0)
                            )
                            <= 1
                        )
                s.check(f"{layout}: all resize corners stay in workspace on {name}")

                # A single tiled window's preview is bounded too. Its native
                # layout still decides whether the requested resize has an effect.
                s.close()
                s.ctl("dispatch", "settiled", "address:" + address)
                s.ctl("dispatch", "hyprspace:overview", "on")
                time.sleep(0.15)
                resize(s, (work["x"] + work["w"] - 1, work["y"] + work["h"] - 1))
                assert inside(window_box(s.windows()["hs-A"]), work)
                s.check(f"{layout}: tiled resize preview stays in workspace on {name}")

        s.setup("dwindle", 0, 2)
        work = area(next(m for m in s.data("monitors") if m["name"] == s.names[0]))
        address = s.windows()["hs-A"]["address"]
        reset_float(s, address, work)
        resize(s, s.preview_point("hs-B", 0.7, 0.7))
        assert s.windows()["hs-A"]["workspace"]["id"] == 11
        assert inside(window_box(s.windows()["hs-A"]), work)
        s.check("resize across another output keeps its source workspace")

        reset_float(s, address, work)
        resize(s, s.preview_point("hs-B"), cancel=True)
        s.close()
        assert all(w["alpha"] == 1 for w in s.status()["windows"])
        s.check("Escape cancels an oversized resize and restores visibility")

        # Native corner overrides and aspect/size rules remain effective.
        for corner in range(1, 5):
            left, top = corner in (1, 4), corner in (1, 2)
            s.ctl("keyword", "general:resize_corner", str(corner))
            reset_float(s, address, work)
            end = (
                work["x"] + (1 if left else work["w"] - 1),
                work["y"] + (1 if top else work["h"] - 1),
            )
            # Deliberately pick up the opposite corner.
            resize(s, end, not left, not top)
            assert inside(window_box(s.windows()["hs-A"]), work)
        s.ctl("keyword", "general:resize_corner", "0")
        s.check("forced resize corners use the same bounds as native resizing")

        s.ctl("dispatch", "setprop", f"address:{address} keep_aspect_ratio on")
        reset_float(s, address, work)
        original = s.windows()["hs-A"]["size"]
        resize(s, s.preview_point("hs-B"))
        actual = s.windows()["hs-A"]["size"]
        assert abs(actual[0] / actual[1] - original[0] / original[1]) < 0.015
        assert inside(window_box(s.windows()["hs-A"]), work)
        s.ctl("dispatch", "setprop", f"address:{address} keep_aspect_ratio off")
        s.check("bounded floating resize preserves an application's aspect ratio")

        s.ctl("dispatch", "setprop", f"address:{address} min_size 180 120")
        s.ctl("dispatch", "setprop", f"address:{address} max_size 300 210")
        reset_float(s, address, work)
        resize(s, (work["x"] + work["w"] - 1, work["y"] + work["h"] - 1))
        assert s.windows()["hs-A"]["size"] == [300, 210]
        reset_float(s, address, work)
        resize(s, (work["x"] + 1, work["y"] + 1))
        assert s.windows()["hs-A"]["size"] == [180, 120]
        s.check("bounded floating resize respects minimum and maximum sizes")

        s.setup("dwindle", 0, 2)
        address = s.windows()["hs-A"]["address"]
        reset_float(s, address, work, (1400, 900))
        resize(s, s.preview_point("hs-B"))
        assert inside(window_box(s.windows()["hs-A"]), work)
        assert s.windows()["hs-A"]["workspace"]["id"] == 11
        s.check("resizing a previously oversized float brings it into its workspace")

        for fullscreen in ("0", "1"):
            reset_float(s, address, work)
            s.close()
            s.ctl("dispatch", "focuswindow", "address:" + address)
            s.ctl("dispatch", "fullscreen", fullscreen)
            s.ctl("dispatch", "hyprspace:overview", "on")
            time.sleep(0.15)
            resize(s, s.preview_point("hs-B"))
            assert s.windows()["hs-A"]["fullscreen"] == 0
            assert inside(window_box(s.windows()["hs-A"]), work)
        s.check(
            "fullscreen and maximized floats leave fullscreen through a bounded native resize"
        )

        s.ctl("keyword", "binds:drag_threshold", "12")
        s.ctl("keyword", "general:snap:enabled", "true")
        reset_float(s, address, work)
        resize(s, s.preview_point("hs-B"))
        assert inside(window_box(s.windows()["hs-A"]), work)
        assert s.windows()["hs-A"]["workspace"]["id"] == 11
        s.ctl("keyword", "binds:drag_threshold", "0")
        s.ctl("keyword", "general:snap:enabled", "false")
        s.check("drag thresholds and native snapping preserve resize bounds")

        s.setup("dwindle", 0, 2)
        address = s.windows()["hs-A"]["address"]
        s.ctl("dispatch", "togglegroup")
        s.ctl(
            "dispatch",
            "movetoworkspacesilent",
            f'11,address:{s.windows()["hs-C"]["address"]}',
        )
        s.ctl("dispatch", "focuswindow", "address:" + s.windows()["hs-C"]["address"])
        first, second = s.point(s.windows()["hs-A"]), s.point(s.windows()["hs-C"])
        dx, dy = first[0] - second[0], first[1] - second[1]
        direction = (
            ("l" if dx < 0 else "r") if abs(dx) > abs(dy) else ("u" if dy < 0 else "d")
        )
        s.ctl("dispatch", "moveintogroup", direction)
        s.ctl("dispatch", "focuswindow", "address:" + address)
        assert len(s.windows()["hs-A"]["grouped"]) == 2
        reset_float(s, address, work)
        resize(s, s.preview_point("hs-B"))
        assert len(s.windows()["hs-A"]["grouped"]) == 2
        for title in ("hs-A", "hs-C"):
            assert inside(window_box(s.windows()[title]), work)
            assert s.windows()[title]["workspace"]["id"] == 11
        s.check("floating groups resize together inside the source workspace")

        s.setup("dwindle", 0, 2)
        address = s.windows()["hs-A"]["address"]
        reset_float(s, address, work)
        s.ctl("dispatch", "setprop", f"address:{address} min_size 1400 1000")
        time.sleep(0.1)
        before = resize(s, s.preview_point("hs-B"), left=True, top=True, bounded=False)
        assert s.geometry() == before
        s.check("an impossible application minimum cancels without changing geometry")

        s.setup("dwindle", 0, 2)
        address = s.windows()["hs-A"]["address"]
        entry = s.root / "resize-panel"
        panel = s.spawn(
            [
                "python3",
                str(Path(__file__).with_name("layer.py")),
                str(entry),
                "waybar",
                "bottom",
            ]
        )
        wait_for(lambda: s.layer("waybar"))
        work = area(next(m for m in s.data("monitors") if m["name"] == s.names[0]))
        reset_float(s, address, work)
        layer = s.layer("waybar")
        resize(s, (layer["x"] + 70, layer["y"] + 50), left=True, top=True)
        assert inside(window_box(s.windows()["hs-A"]), work)
        assert not Path(str(entry) + ".click").exists()
        panel.terminate()
        panel.wait(timeout=3)
        s.check("an active resize keeps pointer capture across a reserved panel")
    finally:
        s.close()
        s.ctl("keyword", "general:resize_corner", "0")
        s.ctl("keyword", "binds:drag_threshold", "0")
        s.ctl("keyword", "general:snap:enabled", "false")
        for name in s.names:
            s.ctl("keyword", "monitor", f"{name},addreserved,0,0,0,0")


def animated(s):
    from PIL import Image

    s.setup("dwindle", 0, 2)
    work = area(next(m for m in s.data("monitors") if m["name"] == s.names[0]))
    reset_float(s, s.windows()["hs-A"]["address"], work)
    s.close()
    s.ctl("keyword", "animations:enabled", "true")
    s.ctl("keyword", "animation", "windowsMove,1,20,default")
    try:
        s.ctl("dispatch", "hyprspace:overview", "on")
        time.sleep(0.1)
        s.move(s.preview_point("hs-A", 0.8, 0.8))
        s.key(125, 1)
        s.button(1, 273)
        assert s.status()["dragging"]
        s.move(s.preview_point("hs-B", 0.9, 0.9))
        time.sleep(2.2)
        clip = s.status()["drag"]["clip"]
        path = s.root / "animated-resize.png"
        s.run("grim", "-s", "1", "-o", s.names[0], str(path))
        # The white fixture must not paint outside the settled workspace. The
        # pointer is on another output, away from these measured pixels.
        image = Image.open(path).convert("RGB")
        pixels = image.load()
        outside = sum(
            min(pixels[x, y]) > 200
            for y in range(image.height)
            for x in range(image.width)
            if not (
                clip["x"] - 2 <= x <= clip["x"] + clip["w"] + 2
                and clip["y"] - 2 <= y <= clip["y"] + clip["h"] + 2
            )
        )
        assert outside == 0, (outside, clip, path)
        s.button(0, 273)
        s.key(125, 0)
        time.sleep(0.12)
        assert inside(window_box(s.windows()["hs-A"]), work)
        assert s.status()["live"] and not s.status()["dragging"]
        s.check(
            "resize started during opening remains clipped as the workspace animates"
        )
    finally:
        s.button(0, 273)
        s.key(125, 0)
        s.ctl("keyword", "animations:enabled", "false")
        s.close()
