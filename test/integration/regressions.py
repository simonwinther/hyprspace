"""Protocol and viewport regressions from the interactive overview audit."""

from pathlib import Path
import re
import subprocess
import time

REPO = Path(__file__).resolve().parents[2]
CLIENT = REPO / "test/integration/client.py"


def offsets(suite):
    return {path: path.stat().st_size for path in suite.root.glob("process-*.log")}


def events(saved, pattern):
    result = []
    for path, offset in saved.items():
        with path.open() as stream:
            stream.seek(offset)
            result.extend(line.strip() for line in stream if re.search(pattern, line))
    return result


def tile(suite, workspace):
    return next(
        tile
        for view in suite.status()["views"]
        for tile in view["tiles"]
        if tile["workspace"] == workspace and tile["window"] == "0x0"
    )


def center(box):
    return box["x"] + box["w"] / 2, box["y"] + box["h"] / 2


def arrow(box, horizontal, forward):
    size = min(34, min(box["w"], box["h"]) / 4)
    inset = min(8, size / 3) + size / 2
    x, y = center(box)
    if horizontal:
        x = box["x"] + (box["w"] - inset if forward else inset)
    else:
        y = box["y"] + (box["h"] - inset if forward else inset)
    return x, y


def audit(s, wait_for):
    s.env["WAYLAND_DEBUG"] = "client"
    try:
        s.setup("dwindle", 0, 1)
    finally:
        s.env.pop("WAYLAND_DEBUG")
    application_logs = tuple(offsets(s))
    s.ctl("dispatch", "hyprspace:overview", "on")
    wait_for(lambda: s.status()["keyboard_owned"])
    time.sleep(0.2)
    saved = offsets(s)
    s.run("wtype", "-M", "ctrl", "-M", "shift", "-k", "q", "-m", "shift", "-m", "ctrl")
    s.ctl("dispatch", "focuswindow", "address:" + s.windows()["hs-B"]["address"])
    s.run("wtype", "-M", "logo", "-k", "j", "-m", "logo")
    time.sleep(0.1)
    assert s.status()["keyboard_owned"] and s.status()["modifiers"] == 0
    assert not events(saved, r"wl_keyboard#\d+\.(enter|modifiers|key)\("), events(
        saved, r"wl_keyboard#"
    )
    s.check(
        "overview blocks application modifier, key and focus-enter events during native commands"
    )

    # A bottom panel must win native hit testing even when a floating or
    # fullscreen application occupies exactly the same desktop coordinates.
    entry = s.root / "overlapping-panel"
    panel = s.spawn(
        [
            "python3",
            str(REPO / "test/integration/layer.py"),
            str(entry),
            "waybar",
            "bottom",
        ]
    )
    wait_for(lambda: s.layer("waybar"))
    box = s.layer("waybar")
    address = s.windows()["hs-A"]["address"]
    s.ctl("dispatch", "setfloating", "address:" + address)
    s.ctl("dispatch", "resizewindowpixel", "exact 600 220,address:" + address)
    s.ctl(
        "dispatch", "movewindowpixel", f'exact {box["x"]} {box["y"]},address:{address}'
    )
    for mode in ("floating", "fullscreen"):
        if mode == "fullscreen":
            s.close()
            s.ctl("dispatch", "focuswindow", "address:" + address)
            s.ctl("dispatch", "fullscreen", "0")
            s.ctl("dispatch", "hyprspace:overview", "on")
        time.sleep(0.2)
        saved = {path: path.stat().st_size for path in application_logs}
        click = Path(str(entry) + ".click")
        click.unlink(missing_ok=True)
        s.move((box["x"] + 70, box["y"] + box["h"] / 2))
        s.button(1)
        s.button(0)
        wait_for(click.exists)
        assert s.status()["live"]
        assert not events(saved, r"wl_pointer#\d+\.(enter|button)\("), events(
            saved, r"wl_pointer#"
        )
        s.check(
            f"promoted bottom panel receives clicks above {mode} windows without application input"
        )
        if mode == "floating":
            s.button(1, 273)
            s.button(0, 273)
            wait_for(lambda: Path(str(entry) + ".popup").exists())
            wait_for(lambda: not s.status()["keyboard_owned"])
            # A keyboard-interactivity:none panel's menu also uses pointer
            # activation on the native desktop.
            s.move((box["x"] + 120, box["y"] + box["h"] / 2 + 14))
            s.button(1)
            s.button(0)
            wait_for(lambda: Path(str(entry) + ".action").exists())
            wait_for(lambda: s.status()["keyboard_owned"])
            assert s.status()["live"]
            assert not events(saved, r"wl_keyboard#\d+\.(enter|key)\(")
            s.check(
                "promoted panel popups retain native grabs and return input to the overview"
            )
    panel.terminate()
    panel.wait(timeout=3)
    s.close()
    s.ctl("dispatch", "focuswindow", "address:" + address)
    s.ctl("dispatch", "fullscreen", "0")
    log = s.root / "input.log"
    log.write_text("")
    s.run("wtype", "z")
    wait_for(lambda: " 122" in log.read_text())
    s.check("application keyboard delivery resumes after overview dismissal")
    s.ctl("dispatch", "hyprspace:overview", "on")
    s.ctl("plugin", "unload", str(REPO / "build/hyprspace.so"))
    log.write_text("")
    s.run("wtype", "z")
    wait_for(lambda: " 122" in log.read_text())
    s.ctl("plugin", "load", str(REPO / "build/hyprspace.so"))
    s.check("unload restores native keyboard focus and delivery")

    ime = s.spawn(
        [str(REPO / "build/test-ime")],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        text=True,
    )
    assert ime.stdout.readline().strip() == "ready"
    s.ctl("dispatch", "hyprspace:overview", "on")
    wait_for(lambda: s.status()["keyboard_owned"])
    assert s.protocol(ime, "reset") == "ok"
    s.run("wtype", "-M", "ctrl", "-M", "shift", "-k", "q", "-m", "shift", "-m", "ctrl")
    assert s.protocol(ime, "counts") == "0 0"
    s.close()
    s.run("wtype", "-M", "shift", "-k", "q", "-m", "shift")
    assert all(int(n) > 0 for n in s.protocol(ime, "counts").split())
    ime.terminate()
    ime.wait(timeout=3)
    s.check(
        "IME keyboard grabs receive no overview keys or modifiers and resume after dismissal"
    )

    s.setup("dwindle", 0, 1)
    s.ctl("keyword", "plugin:hyprspace:overview:all_monitors", "false")
    try:
        s.move(s.point(s.windows()["hs-A"]))
        s.ctl("dispatch", "hyprspace:overview", "on")
        address = s.windows()["hs-A"]["address"]
        wait_for(lambda: len(s.status()["views"]) == 1)
        assert s.status()["views"][0]["monitor"] == "WAYLAND-1"

        def alpha():
            return next(
                w["alpha"] for w in s.status()["windows"] if w["address"] == address
            )

        wait_for(lambda: alpha() == 0)
        for workspace, expected in ((12, 1), (11, 0), (13, 1), (11, 0), (12, 1)):
            s.ctl("dispatch", "movetoworkspacesilent", f"{workspace},address:{address}")
            wait_for(lambda: alpha() == expected)
            assert s.status()["live"] and len(s.status()["views"]) == 1
        s.close()
        assert all(w["alpha"] == 1 for w in s.status()["windows"])
        s.check(
            "single-monitor visibility is released and reacquired across repeated workspace moves"
        )
    finally:
        s.close()
        s.ctl("keyword", "plugin:hyprspace:overview:all_monitors", "true")

    s.setup("dwindle", 0, 1)
    s.ctl("dispatch", "hyprspace:overview", "on")
    time.sleep(0.2)
    s.move(s.preview_point("hs-B"))
    token = s.request("capture")
    assert token
    s.ctl("dispatch", "moveworkspacetomonitor", "12 WAYLAND-1")
    s.ctl("keyword", "monitor", "WAYLAND-2,disable")
    s.await_outputs(2)
    try:
        s.move(center(tile(s, 13)))
        s.spawn(
            [
                str(REPO / "build/hyprspace-launch"),
                "--context",
                token,
                "--",
                "sh",
                "-c",
                f"sleep .3; exec python3 {CLIENT} hs-transfer-disconnect",
            ]
        )
        wait_for(lambda: "hs-transfer-disconnect" in s.windows())
        w = s.windows()["hs-transfer-disconnect"]
        assert w["workspace"]["id"] == 12
        assert w["monitor"] == next(
            m["id"] for m in s.data("monitors") if m["name"] == "WAYLAND-1"
        )
        assert s.status()["live"] and s.status()["layout_targets_unique"]
        s.check(
            "delayed launch survives workspace transfer and removal of its originally captured output"
        )
    finally:
        s.ctl("keyword", "monitor", "WAYLAND-2,960x600@60,-1000x-200,1.25,transform,1")
        s.await_outputs(3)
        s.close()


def scrolling(s, wait_for):
    s.ctl("keyword", "scrolling:focus_fit_method", "1")
    for monitor in range(3):
        for direction in ("right", "left", "down", "up"):
            s.ctl("keyword", "scrolling:direction", direction)
            s.setup("scrolling", monitor, monitor)
            horizontal = direction in ("right", "left")
            axis = 0 if horizontal else 1
            first = min(s.windows(), key=lambda title: s.windows()[title]["at"][axis])
            s.ctl("dispatch", "focuswindow", "address:" + s.windows()[first]["address"])
            s.ctl("dispatch", "hyprspace:overview", "on")
            time.sleep(0.2)
            box = tile(s, 11 + monitor)
            s.move(center(box))
            focused = s.data("activewindow")["address"]

            def position():
                return s.windows()[first]["at"][axis]

            before = position()
            s.scroll()
            wait_for(lambda: position() < before - 1)
            full_step = before - position()
            before = position()
            s.scroll(delta=-3.75, discrete=0, axis=1)
            wait_for(lambda: position() > before + 1)
            assert abs((position() - before) / full_step - 0.25) < 0.04, (
                direction,
                monitor,
                full_step,
                position() - before,
            )
            before = position()
            s.scroll(delta=-4, discrete=0, axis=0, source=1)
            wait_for(lambda: position() > before + 1)
            after = position()
            s.scroll(delta=0, discrete=0, axis=0, source=1)
            time.sleep(0.1)
            assert position() == after
            assert s.status()["target"]["workspace"] == 11 + monitor
            assert s.data("activewindow")["address"] == focused
            assert s.status()["live"] and s.status()["keyboard_owned"]
            # Bound both ends, including fractional-scale portrait geometry.
            for sign in (1, -1):
                for _ in range(12):
                    s.scroll(delta=15 * sign, discrete=sign)
                endpoint = position()
                s.scroll(delta=15 * sign, discrete=sign)
                assert position() == endpoint
            s.move(arrow(box, horizontal, True))
            before = position()
            s.button(1)
            s.button(0)
            wait_for(lambda: position() < before - 1)
            assert s.status()["live"]
            selected = s.status()["target"]["window"]
            assert selected != "0x0"
            time.sleep(0.1)
            assert s.status()["target"]["window"] == selected
            s.run("wtype", "-k", "Page_Up")
            assert s.status()["target"]["window"] != selected
            s.run("wtype", "-k", "Page_Down")
            assert s.status()["target"]["window"] == selected
            s.run("wtype", "-k", "Return")
            wait_for(lambda: not s.status()["live"])
            assert s.data("activewindow")["address"] == selected
            s.check(
                f"scrolling {direction} on output {monitor+1}: wheel, fractional wheel, touchpad, bounds, arrows and keyboard"
            )
    s.ctl("keyword", "scrolling:direction", "right")

    # Pan an inactive workspace while a foreground layer owns typing.
    s.setup("scrolling", 1, 1)
    s.ctl("dispatch", "focusmonitor", "WAYLAND-2")
    s.ctl("dispatch", "workspace", "22")
    s.ctl("dispatch", "hyprspace:overview", "on")
    time.sleep(0.2)
    box = tile(s, 12)
    entry = s.root / "scroll-layer"
    layer = s.spawn(["python3", str(REPO / "test/integration/layer.py"), str(entry)])
    wait_for(lambda: s.layer("hs-foreground"))
    s.move(center(box))
    before = s.geometry()
    s.scroll()
    wait_for(lambda: s.geometry() != before)
    assert (
        next(m for m in s.data("monitors") if m["name"] == "WAYLAND-2")[
            "activeWorkspace"
        ]["id"]
        == 22
    )
    assert not s.status()["keyboard_owned"]
    s.run("wtype", "viewport typing")
    wait_for(lambda: entry.exists() and entry.read_text() == "viewport typing")
    s.move(arrow(box, True, True))
    s.button(1)
    s.button(0)
    assert s.status()["live"] and not s.status()["keyboard_owned"]
    s.run("wtype", " after arrow")
    wait_for(lambda: entry.read_text() == "viewport typing after arrow")
    s.check(
        "pointer scrolling of an inactive workspace preserves foreground keyboard focus"
    )
    layer.terminate()
    layer.wait(timeout=3)
    s.close()

    for fit in (0, 1):
        s.ctl("keyword", "scrolling:focus_fit_method", str(fit))
        s.setup("scrolling", 0, 0)
        s.ctl("keyword", "workspace", "11,layoutopt:direction:down")
        s.ctl("dispatch", "hyprspace:overview", "on")
        time.sleep(0.2)
        box = tile(s, 11)
        s.move(center(box))
        s.scroll()
        s.scroll()
        before = s.geometry()
        for _ in range(12):
            s.scroll(delta=-15, discrete=-1)
        assert s.geometry() != before
        s.move(arrow(box, False, True))
        before = s.geometry()
        s.button(1)
        s.button(0)
        assert s.status()["live"] and s.geometry() != before
        s.check(
            f"workspace direction override and native fit method {fit} control viewport navigation"
        )
        s.close()
        s.ctl("keyword", "workspace", "11,layoutopt:direction:")
    s.ctl("keyword", "scrolling:focus_fit_method", "1")

    s.setup("scrolling", 0, 0)
    s.ctl("dispatch", "hyprspace:overview", "on")
    time.sleep(0.2)
    s.move(center(tile(s, 11)))
    s.scroll()
    first = next(iter(s.windows()))
    before = s.windows()[first]["at"][0]
    s.ctl("keyword", "input:scroll_factor", "0.5")
    s.scroll(delta=-15, discrete=-1)
    half = s.windows()[first]["at"][0] - before
    s.ctl("keyword", "input:scroll_factor", "1")
    s.scroll()
    before = s.windows()[first]["at"][0]
    s.scroll(delta=-15, discrete=-1)
    full = s.windows()[first]["at"][0] - before
    assert abs(half / full - 0.5) < 0.03
    s.check("overview wheel speed respects the compositor scroll factor")
    s.close()

    s.setup("dwindle", 0, 0)
    s.ctl("keyword", "workspace", "21,monitor:WAYLAND-1,persistent:true,layout:master")
    s.ctl("dispatch", "hyprspace:overview", "on")
    time.sleep(0.2)
    s.move(center(tile(s, 11)))
    before = s.geometry()
    selected = s.status()["target"]["workspace"]
    s.scroll(axis=1)
    assert s.geometry() == before and s.status()["target"]["workspace"] == selected
    s.scroll()
    assert s.geometry() == before and s.status()["target"]["workspace"] != selected
    s.run("wtype", "-M", "shift", "-k", "Tab", "-m", "shift")
    assert s.status()["target"]["workspace"] == selected
    s.check(
        "dwindle and master retain wheel tile navigation and Shift+Tab returns to the previous tile"
    )
    s.close()

    # Real animation frames must refresh the stationary pointer's window hit.
    s.setup("scrolling", 0, 0)
    s.ctl("keyword", "animations:enabled", "true")
    try:
        first = min(s.windows(), key=lambda title: s.windows()[title]["at"][0])
        s.ctl("dispatch", "focuswindow", "address:" + s.windows()[first]["address"])
        time.sleep(0.6)
        s.ctl("dispatch", "hyprspace:overview", "on")
        time.sleep(0.6)
        box = tile(s, 11)
        point = center(box)
        s.move(point)
        before = s.geometry()
        s.scroll()
        wait_for(lambda: s.geometry() != before)
        time.sleep(0.8)
        s.run(
            "grim", "-s", "1", "-o", "WAYLAND-1", str(s.root / "scrolling-controls.png")
        )
        hit = next(
            t
            for v in s.status()["views"]
            for t in v["tiles"]
            if t["window"] != "0x0"
            and t["x"] < point[0] < t["x"] + t["w"]
            and t["y"] < point[1] < t["y"] + t["h"]
        )
        assert s.status()["target"]["window"] == hit["window"]
        s.button(1)
        s.button(0)
        wait_for(lambda: not s.status()["live"])
        assert s.data("activewindow")["address"] == hit["window"]
        s.check(
            "animated scrolling refreshes stationary pointer targeting and window clicks still focus and close"
        )
    finally:
        s.close()
        s.ctl("keyword", "animations:enabled", "false")
