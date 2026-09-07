"""Switcher visibility over ordinary and optimized fullscreen frames."""

import time

from PIL import Image


def visibility(s, wait_for):
    def monitor(index):
        return next(m for m in s.data("monitors") if m["name"] == s.names[index])

    def panel_pixels(index, name):
        path = s.root / f"switcher-{name}-{index + 1}.png"
        s.run("grim", "-s", "1", "-o", s.names[index], str(path))
        with Image.open(path) as image:
            colors = image.convert("RGB").getcolors(image.width * image.height)
        return sum(n for n, (r, g, b) in colors if r > 245 and g < 10 and b > 245)

    def press(key):
        s.key(key, 1)
        s.key(key, 0)

    def open_switcher(backward=False):
        s.key(56, 1)  # Alt stays down while the panel is visible.
        if backward:
            s.key(42, 1)
        press(15)  # Tab
        if backward:
            s.key(42, 0)
        wait_for(lambda: s.status()["cursor_owned"])

    def cancel():
        press(1)  # Escape must preserve the fullscreen window and focus.
        s.key(56, 0)
        wait_for(lambda: not s.status()["cursor_owned"])

    # Count a distinctive panel color instead of accepting input-only success.
    s.ctl("keyword", "plugin:hyprspace:switcher:bg_color", "rgba(ff00ffff)")
    try:
        for index in range(3):
            for mode in ("tiled", "fullscreen", "maximized"):
                s.setup("dwindle", index, index)
                # The startup notification disables the fullscreen shortcut and
                # would hide this regression even when the switcher is broken.
                s.ctl("dismissnotify", "-1")
                address = s.windows()["hs-A"]["address"]
                if mode != "tiled":
                    s.ctl("dispatch", "fullscreen", "0" if mode == "fullscreen" else "1")
                if mode == "fullscreen" and index == 0:
                    wait_for(lambda: int(monitor(index)["solitary"], 16) != 0)
                for backward in (False, True):
                    open_switcher(backward)
                    assert panel_pixels(index, f"{mode}-{backward}") > 1000, (
                        "switcher is hidden", mode, index, backward, monitor(index)
                    )
                    press(15)
                    assert panel_pixels(index, f"{mode}-{backward}-next") > 1000
                    assert s.data("activewindow")["address"] == address
                    cancel()
                    assert panel_pixels(index, f"{mode}-{backward}-closed") == 0
                    assert s.data("activewindow")["address"] == address
                    if mode == "fullscreen":
                        assert s.windows()["hs-A"]["fullscreen"] == 2
                        if index == 0:
                            wait_for(lambda: int(monitor(index)["solitary"], 16) != 0)
                open_switcher()
                s.key(56, 0)
                wait_for(lambda: s.data("activewindow")["address"] != address)
                wait_for(lambda: not s.status()["cursor_owned"])
                s.check(f"{mode} on output {index + 1}: visible Alt+Tab, reverse, advance, cancel and commit")

        # GTK's opaque region at fractional scale can block the optimization.
        # Use two scale-1 outputs to verify that inhibition stays per monitor.
        s.ctl("keyword", "monitor", f"{s.names[1]},960x600@60,-1000x-200,1,transform,0")
        wait_for(lambda: monitor(1)["scale"] == 1 and monitor(1)["transform"] == 0)
        s.setup("dwindle", 0, 1)
        s.ctl("dispatch", "fullscreen", "0")
        s.ctl("dispatch", "focuswindow", "address:" + s.windows()["hs-B"]["address"])
        s.ctl("dispatch", "fullscreen", "0")
        s.move(s.point(s.windows()["hs-B"], 0.1, 0.1))
        wait_for(lambda: all(int(monitor(i)["solitary"], 16) != 0 for i in (0, 1)))
        open_switcher()
        assert panel_pixels(1, "two-fullscreen") > 1000
        assert panel_pixels(0, "unaffected") == 0
        assert int(monitor(0)["solitary"], 16) != 0
        cancel()
        wait_for(lambda: all(int(monitor(i)["solitary"], 16) != 0 for i in (0, 1)))
        s.check("fullscreen optimization remains available on other outputs and resumes after dismissal")

        s.ctl("keyword", "animations:enabled", "true")
        open_switcher()
        time.sleep(0.6)
        assert panel_pixels(1, "animated") > 1000
        cancel()
        wait_for(lambda: int(monitor(1)["solitary"], 16) != 0)
        s.check("animated switcher stays above fullscreen until its closing animation finishes")
    finally:
        s.key(42, 0)
        s.key(56, 0)
        s.close()
        s.ctl("keyword", "animations:enabled", "false")
        s.ctl("keyword", "plugin:hyprspace:switcher:bg_color", "rgba(1e1e2ef0)")
        s.ctl("keyword", "monitor", f"{s.names[1]},960x600@60,-1000x-200,1.25,transform,1")
