"""Read native Board caption pixels under opposite control/backdrop themes."""

import json

from PIL import Image


def caption_corners(image, board, scale, surface):
    # Find the actual caption's solid surface in native screenshot pixels. Its
    # bounding extent comes from rendered pixels, not a second card-layout model.
    rgb = image.convert("RGB")
    width, height = rgb.size
    pixels = rgb.tobytes()
    target = bytes(surface)
    remaining = bytearray(pixels[index:index + 3] == target for index in range(0, len(pixels), 3))
    viewport = board["viewport"]
    minimum = round(viewport["y"] * scale)
    maximum = round((viewport["y"] + viewport["h"]) * scale)
    components = []
    for seed in range(minimum * width, min(maximum, height) * width):
        if not remaining[seed]:
            continue
        remaining[seed] = 0
        pending = [seed]
        left = right = seed % width
        top = bottom = seed // width
        count = 0
        while pending:
            point = pending.pop()
            x, y = point % width, point // width
            count += 1
            left, right = min(left, x), max(right, x)
            top, bottom = min(top, y), max(bottom, y)
            neighbors = []
            if x:
                neighbors.append(point - 1)
            if x + 1 < width:
                neighbors.append(point + 1)
            if y:
                neighbors.append(point - width)
            if y + 1 < height:
                neighbors.append(point + width)
            for neighbor in neighbors:
                if remaining[neighbor]:
                    remaining[neighbor] = 0
                    pending.append(neighbor)
        if (right - left > 200 and 20 < bottom - top < 100 and top >= minimum and bottom < maximum
                and count > (right - left) * (bottom - top) * .6):
            components.append((left, top, right, bottom))
    assert len(components) >= 2, components
    # Home selects the first card; the pointer is outside this viewport.
    left, top, right, bottom = max(components, key=lambda box: box[0])
    result = {}
    for name, point, reference in (
        ("left", (left + 1, bottom - 1), (left - 6, bottom - 1)),
        ("right", (right - 1, bottom - 1), (right + 6, bottom - 1)),
    ):
        actual, backdrop = rgb.getpixel(point), rgb.getpixel(reference)
        result[name] = {"pixel": point, "rgb": actual, "backdrop_pixel": reference, "backdrop_rgb": backdrop,
                        "matches_backdrop": actual == backdrop, "outside_surface": actual != surface}
    interior = (right - 24, top + 6)
    return {"caption_bounds": [left, top, right, bottom],
            "interior": {"pixel": interior, "rgb": rgb.getpixel(interior), "expected_rgb": surface},
            "corners": result}


def run(s, wait_for):
    s.setup("dwindle", 0, 0)
    original = s.data("monitors")
    name = s.names[0]
    reports = {}
    try:
        s.ctl("keyword", "monitor", f"{name},1920x1080@60,0x0,1.2,transform,0")
        wait_for(lambda: (monitor := next(value for value in s.data("monitors") if value["name"] == name))["width"] == 1920
                 and abs(monitor["scale"] - 1.2) < .001)
        s.ctl("keyword", "plugin:hyprspace:overview:font", "Sans 12")
        s.ctl("keyword", "plugin:hyprspace:overview:bg_dim", "1")
        # Make the rounded contour unambiguous even if the suite's defaults change.
        s.ctl("keyword", "plugin:hyprspace:overview:rounding", "14")
        s.ctl("keyword", "animations:enabled", "false")
        s.move((1800, 950))  # Keep the pointer outside the first output's cards.
        for theme, background, label, surface in (
            ("light", "111119ff", "cdd6f4ff", (244, 245, 246)),
            ("dark", "eff1f5ff", "303446ff", (22, 23, 27)),
        ):
            s.close()
            s.ctl("keyword", "plugin:hyprspace:overview:bg_color", f"rgba({background})")
            s.ctl("keyword", "plugin:hyprspace:overview:label_color", f"rgba({label})")
            s.ctl("keyword", "plugin:hyprspace:spaces:theme", theme)
            s.ctl("dispatch", "focusmonitor", name)
            s.ctl("dispatch", "hyprspace:overview", "on")
            wait_for(lambda: s.status()["live"] and s.status()["keyboard_owned"])
            s.ctl("dispatch", "hyprspace:windowview", "flat")
            wait_for(lambda: (board := s.status()["window_board"]) and board["active"] and board["count"] == 3)
            s.key(102, 1)  # Home: keep the sampled far-right card unselected.
            s.key(102, 0)
            path = s.root / f"control-surfaces-{theme}.png"
            scale = next(value["scale"] for value in s.data("monitors") if value["name"] == name)
            s.run("grim", "-s", str(scale), "-o", name, str(path))
            board = s.status()["window_board"]
            with Image.open(path) as image:
                report = caption_corners(image, board, scale, surface)
            reports[theme] = report
            (s.root / "control-surfaces-pixels.json").write_text(json.dumps(reports, indent=2))
            assert report["interior"]["rgb"] == surface, report
            assert all(value["matches_backdrop"] and value["outside_surface"] for value in report["corners"].values()), report
            s.check(f"forced {theme} Board caption retains its surface inside and the opposite backdrop outside both rounded bottom corners")
    finally:
        s.close()
        s.ctl("reload")
        for monitor in original:
            s.ctl("keyword", "monitor", f'{monitor["name"]},{monitor["width"]}x{monitor["height"]}@{monitor["refreshRate"]},'
                  f'{monitor["x"]}x{monitor["y"]},{monitor["scale"]},transform,{monitor["transform"]}')
        s.await_outputs(3)
        assert s.ctl("configerrors") == ""
