"""Real high-resolution capture pixels, child surfaces and frame callbacks."""

import json

from resize import area


def _frame_count(path):
    frames = [line.split() for line in path.read_text().splitlines() if line.startswith("frame ")]
    return int(frames[-1][1]) if frames else 0


def _child_color(color):
    red, green, blue, alpha = color
    if alpha != 255:
        return None
    if green > 2 * red and blue > 2 * red and abs(green - blue) < 60:
        return "cyan"
    if red > 2 * green and blue > 2 * green and abs(red - blue) < 60:
        return "magenta"
    return None


def _corners(report):
    samples = report["samples"]
    red, green, blue, alpha = samples["top_left"]
    assert alpha == 255 and red > 2 * max(green, blue), report
    red, green, blue, alpha = samples["top_right"]
    assert alpha == 255 and green > 2 * max(red, blue), report
    red, green, blue, alpha = samples["bottom_left"]
    assert alpha == 255 and blue > 2 * max(red, green), report
    red, green, blue, alpha = samples["bottom_right"]
    assert alpha == 255 and min(red, green) > 2 * blue and abs(red - green) < 60, report
    assert _child_color(samples["child"]), report


def _clipped_corners(report):
    samples = report["samples"]
    red, green, blue, alpha = samples["top_left"]
    assert alpha == 255 and red > 2 * max(green, blue), report
    for corner in ("top_right", "bottom_left", "bottom_right"):
        assert samples[corner] == [0, 0, 0, 0], report
    assert _child_color(samples["child"]), report


def coverage(s, wait_for):
    fixture = s.artifact("test-overview.so")
    client = s.artifact("test-capture-client")
    assert fixture.is_file() and client.is_file(), "build integration-fixtures before the capture suite"
    original_monitors = s.data("monitors")
    original_animations = s.data("getoption animations:enabled")["int"]
    measurements = {}
    s.close()
    s.ctl("plugin", "load", str(fixture))
    # Loading a native plugin schedules a configuration reload; let it finish
    # before creating fixture rules and output geometry.
    import time
    time.sleep(0.3)
    s.ctl("keyword", "windowrule", "match:title ^hs-capture-colors-.*$, float on")
    s.ctl("keyword", "animations:enabled", "false")
    try:
        for index, transform, scale in ((0, 0, 1), (1, 1, 1.25)):
            label = "ordinary" if not transform else "portrait-fractional"
            name = s.names[index]
            title = f"hs-capture-colors-{label}"
            s.setup("dwindle", index, index)
            # Distinct positions keep native pointer routing unambiguous even
            # when a preceding suite left a different monitor arrangement.
            for output, position in zip(s.names, ("0x0", "-1000x-200", "1200x300")):
                s.ctl("keyword", "monitor", f"{output},960x600@60,{position},1,transform,0")
            position = "0x0" if not transform else "-1000x-200"
            s.ctl("keyword", "monitor", f"{name},960x600@60,{position},{scale},transform,{transform}")
            wait_for(lambda: (m := next(m for m in s.data("monitors") if m["name"] == name))["transform"] == transform
                     and abs(m["scale"] - scale) < 0.001)
            s.ctl("dispatch", "focusmonitor", name)
            s.ctl("dispatch", "workspace", str(11 + index))
            bounds = area(next(m for m in s.data("monitors") if m["name"] == name))
            s.move((bounds["x"] + bounds["w"] / 2, bounds["y"] + bounds["h"] / 2))
            log = s.root / f"capture-{label}-frames.log"
            with log.open("w") as output:
                process = s.spawn([str(client), title], stdout=output)
            try:
                window = wait_for(lambda: s.windows().get(title))
                address = window["address"]
                s.ctl("dispatch", "setfloating", "address:" + address)
                s.ctl("dispatch", "resizewindowpixel", f"exact 5120 2880,address:{address}")
                wait_for(lambda: s.windows()[title]["size"] == [5120, 2880])
                wait_for(lambda: _frame_count(log) >= 2)
                before = s.geometry()
                downsampled = s.status()["resources"]["captures"]["downsampled"]
                s.ctl("dispatch", "hyprspace:overview", "on")

                def probe(mode=""):
                    value = json.loads(s.run("hyprctl", "dispatch", "hyprspace-test:capture", address + mode))
                    return value if value["ready"] else None

                report = wait_for(probe)
                (s.root / f"capture-{label}-pixels.json").write_text(json.dumps(report, indent=2))
                s.run("grim", "-s", "1", "-o", name, str(s.root / f"capture-{label}.png"))
                assert report["logical"] == [5120, 2880], report
                assert report["monitor"] == name and abs(report["scale"] - scale) < 0.001, report
                assert report["capture_bytes"] <= 32 * 1024 * 1024, report
                assert report["total_bytes"] <= 256 * 1024 * 1024, report
                assert report["texture"][0] < 5120 * scale and report["texture"][1] < 2880 * scale, report
                assert abs(report["texture"][0] / report["texture"][1] - 5120 / 2880) < 0.002, report
                assert report["downsampled"] > downsampled and report["gl_error"] == 0, report
                _corners(report)
                assert s.geometry() == before, "capture resolution changed desktop input geometry"
                s.check(f"{label}: bounded 5K capture preserves four corners and a far-offset child subsurface")

                clipped = probe(" clipped")
                (s.root / f"capture-{label}-clipped.json").write_text(json.dumps(clipped, indent=2))
                _clipped_corners(clipped)
                assert clipped["gl_error"] == 0 and clipped["surface"]["visible"] == report["surface"]["visible"], clipped

                def restored_corners():
                    current = probe()
                    if current and all(current["samples"][corner][3] == 255 for corner in
                                       ("top_left", "top_right", "bottom_left", "bottom_right")):
                        return current
                    return None

                restored = wait_for(restored_corners)
                _corners(restored)
                assert restored["gl_error"] == 0 and s.geometry() == before, restored
                s.check(f"{label}: source visible-region clipping survives downsampling and restores before the next desktop frame")

                start_frames = _frame_count(log)
                color = _child_color(restored["samples"]["child"])

                def repainted():
                    current = probe()
                    if current and _child_color(current["samples"]["child"]) not in (None, color):
                        return current
                    return None

                changed = wait_for(repainted)
                wait_for(lambda: _frame_count(log) >= start_frames + 12)
                _corners(changed)
                assert changed["gl_error"] == 0 and s.geometry() == before, changed
                s.check(f"{label}: hidden desktop content receives frame callbacks and repaints while downsampled")
                s.run("grim", "-s", "1", "-o", name, str(s.root / f"capture-{label}.png"))
                measurements[label] = {"first": report, "clipped": clipped, "restored": restored,
                                       "repainted": changed, "frames": _frame_count(log) - start_frames}
                (s.root / "capture-pixels.json").write_text(json.dumps(measurements, indent=2))
            finally:
                s.close()
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)
                wait_for(lambda: title not in s.windows())
        wait_for(lambda: s.status()["resources"]["captures"]["bytes"] == 0)
        s.check("high-resolution capture teardown releases every capture framebuffer")
    finally:
        s.close()
        s.ctl("plugin", "unload", str(fixture))
        s.ctl("reload")
        for monitor in original_monitors:
            s.ctl("keyword", "monitor", f'{monitor["name"]},{monitor["width"]}x{monitor["height"]}@{monitor["refreshRate"]},'
                  f'{monitor["x"]}x{monitor["y"]},{monitor["scale"]},transform,{monitor["transform"]}')
        s.ctl("keyword", "animations:enabled", str(bool(original_animations)).lower())
        s.await_outputs(3)
        assert s.ctl("configerrors") == ""
