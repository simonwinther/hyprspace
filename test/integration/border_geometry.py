"""Read actual GPU focus-ring edges, including the old native damage clipping."""

import json
import math
import time

from PIL import Image


def edges(image, box, width, radius):
    """Check every pixel along each outer straight edge, away from corners."""
    x, y, w, h = (box[key] for key in ("x", "y", "w", "h"))
    # Pixel centers must lie inside the projected outer boundary. In particular,
    # the last center can be beyond the old independently-truncated Pixman box.
    left, top = math.ceil(x - width - .5), math.ceil(y - width - .5)
    right = math.ceil(x + w + width - .5) - 1
    bottom = math.ceil(y + h + width - .5) - 1
    xs = range(math.ceil(x + radius + 3), math.floor(x + w - radius - 3))
    ys = range(math.ceil(y + radius + 3), math.floor(y + h - radius - 3))
    points = {
        "top": [(i, top) for i in xs],
        "right": [(right, i) for i in ys],
        "bottom": [(i, bottom) for i in xs],
        "left": [(left, i) for i in ys],
    }
    result = {}
    for name, coordinates in points.items():
        assert len(coordinates) >= 8, (name, box, radius)
        colors = [image.getpixel(point) for point in coordinates]
        # A cyan focus border is distinct from both opaque dark backgrounds.
        visible = [min(green, blue) - red > 100 for red, green, blue in colors]
        result[name] = {"visible": sum(visible), "samples": len(visible),
                        "coverage": sum(visible) / len(visible),
                        "first": coordinates[0], "last": coordinates[-1],
                        "middle_color": colors[len(colors) // 2]}
    return result


def run(s, wait_for):
    fixture = s.artifact("test-overview.so")
    assert fixture.is_file(), "build integration-fixtures before the border suite"
    original = s.data("monitors")
    s.close()
    s.ctl("plugin", "load", str(fixture))
    # Loading a native fixture schedules a configuration reload.
    time.sleep(.3)
    s.ctl("keyword", "animations:enabled", "false")
    s.ctl("dismissnotify", "-1")
    measurements = {}
    try:
        for name, position in zip(s.names, ("0x0", "1500x0", "2500x0")):
            s.ctl("keyword", "monitor", f"{name},960x600@60,{position},1,transform,0")
        # The first case is the user's actual fractional scale. The identical
        # pair also covers ordinary, other fractional, and rotated output paths.
        for scale, transform, index in ((1.2, 0, 0), (1, 0, 0), (1.25, 0, 0), (1.5, 0, 0), (1.25, 1, 1)):
            name = s.names[index]
            label = f"{scale:g}" + ("-rotated" if transform else "")
            position = "1500x0" if index else "0x0"
            s.ctl("keyword", "monitor", f"{name},960x600@60,{position},{scale},transform,{transform}")
            wait_for(lambda: (m := next(m for m in s.data("monitors") if m["name"] == name))["transform"] == transform
                     and abs(m["scale"] - scale) < .001)
            s.move((2800, 450))  # Keep the software cursor away from the rings.
            report = json.loads(s.run("hyprctl", "dispatch", "hyprspace-test:border-geometry", name))
            assert abs(report["scale"] - scale) < .001 and report["transform"] == transform, report
            path = s.root / f"border-geometry-{label}.png"

            def capture():
                # grim's default/scale=1 can resample fractional outputs. Keep
                # one screenshot pixel per physical output pixel instead.
                s.run("grim", "-s", str(scale), "-o", name, str(path))
                with Image.open(path) as image:
                    image = image.convert("RGB")
                    current = {kind: edges(image, report[kind], report["width"], report["radius"])
                               for kind in ("raw", "fixed")}
                    current["size"] = image.size
                return current if current["fixed"]["top"]["coverage"] > .95 else None

            pixels = wait_for(capture)
            report["pixels"] = pixels
            measurements[label] = report
            (s.root / "border-geometry-pixels.json").write_text(json.dumps(measurements, indent=2))
            # This is deliberately A/B: the untreated native box must reproduce
            # the missing far edges, proving that a passing fixed control is not
            # an invisible/no-op render or a fixture that missed the bad phase.
            assert pixels["raw"]["top"]["coverage"] > .95 and pixels["raw"]["left"]["coverage"] > .95, report
            assert pixels["raw"]["bottom"]["coverage"] < .05 and pixels["raw"]["right"]["coverage"] < .05, report
            assert all(edge["coverage"] > .95 for edge in pixels["fixed"].values()), report
            s.check(f"scale {label}: native raw focus ring clips far edges; snapped ring has four continuous GPU edges")
            s.ctl("dispatch", "hyprspace-test:border-geometry", "off")
    finally:
        s.ctl("dispatch", "hyprspace-test:border-geometry", "off")
        s.ctl("plugin", "unload", str(fixture))
        s.ctl("reload")
        for monitor in original:
            s.ctl("keyword", "monitor", f'{monitor["name"]},{monitor["width"]}x{monitor["height"]}@{monitor["refreshRate"]},'
                  f'{monitor["x"]}x{monitor["y"]},{monitor["scale"]},transform,{monitor["transform"]}')
        s.await_outputs(3)
        assert s.ctl("configerrors") == ""
