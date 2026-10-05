"""Frozen launch identity and bounded floating placement in a private compositor."""

import math
from pathlib import Path
import subprocess
import time

from protocol import reply
from regressions import tile
from resize import area


PREFIX = "hs-launch-robust-"


def open_overview(s, wait_for):
    s.ctl("dispatch", "hyprspace:overview", "on")
    wait_for(lambda: len(s.status()["views"]) == len(s.names))
    time.sleep(0.15)


def capture(s, workspace, x=0.5, y=0.5):
    box = tile(s, workspace)
    s.move((box["x"] + box["w"] * x, box["y"] + box["h"] * y))
    target = s.status()["target"]
    assert target["workspace"] == workspace, target
    token = s.request("capture")
    assert token
    return token, target


def activation_client(s):
    app = s.spawn(
        [str(s.artifact("test-activation"))],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        text=True,
    )
    assert reply(app) == "ready"
    return app


def check_placement(s, wait_for, title, workspace, point, border):
    window = wait_for(
        lambda: s.windows().get(title)
        if s.windows().get(title, {}).get("workspace", {}).get("id") == workspace
        else None
    )
    monitor = next(m for m in s.data("monitors") if m["id"] == window["monitor"])
    work = area(monitor)
    width, height = window["size"]
    low = (math.ceil(work["x"] + border), math.ceil(work["y"] + border))
    high = (
        math.floor(work["x"] + work["w"] - border) - width,
        math.floor(work["y"] + work["h"] - border) - height,
    )
    expected = (
        max(low[0], min(high[0], point["x"] - width / 2)),
        max(low[1], min(high[1], point["y"] - height / 2)),
    )
    assert window["floating"], window
    assert all(abs(a - b) <= 1 for a, b in zip(window["at"], expected)), (
        title,
        window,
        expected,
        point,
        work,
    )
    assert s.status()["layout_targets_unique"]
    return window


def delayed(s, wait_for, capture_token, title):
    gate = s.root / (title + ".gate")
    ready = Path(str(gate) + ".ready")
    gate.unlink(missing_ok=True)
    ready.unlink(missing_ok=True)
    program = (
        "import os,sys,time\nfrom pathlib import Path\n"
        "gate=Path(sys.argv[1]); Path(str(gate)+'.ready').touch()\n"
        "while not gate.exists(): time.sleep(.01)\n"
        "os.execv(sys.executable,[sys.executable,*sys.argv[2:]])\n"
    )
    process = s.spawn(
        [
            str(s.artifact("hyprspace-launch")),
            "--context",
            capture_token,
            "--",
            "python3",
            "-c",
            program,
            str(gate),
            str(s.client),
            title,
        ]
    )
    wait_for(ready.exists)
    assert process.poll() is None
    assert s.request("consume " + capture_token) == ""
    return gate


def geometry(s, wait_for, border):
    for index, name in enumerate(s.names):
        s.setup("dwindle", index, (index + 1) % 3)
        app = activation_client(s)
        open_overview(s, wait_for)
        for count, (x, y) in enumerate(
            ((0.5, 0.5), (0.02, 0.02), (0.98, 0.02), (0.98, 0.98), (0.02, 0.98),
             (0.02, 0.5), (0.98, 0.5), (0.5, 0.02), (0.5, 0.98))
        ):
            workspace = 11 + index
            token, point = capture(s, workspace, x, y)
            activation = s.request("consume " + token)
            assert activation
            title = f"{PREFIX}geometry-{index}-{count}"
            # Move away before the exact surface maps. Placement must use the
            # captured desktop point, including negative/portrait coordinates.
            capture(s, 11 + (index + 1) % 3)
            assert s.protocol(app, f"before {title} {activation}") == "ok"
            window = check_placement(s, wait_for, title, workspace, point, border)
            assert window["size"] == [320, 240], window
        s.check(f"launches retain exact cursor placement at center, edges and corners on {name}")


def identity(s, wait_for, border):
    s.setup("dwindle", 0, 1)
    open_overview(s, wait_for)
    first, first_point = capture(s, 11, 0.02, 0.02)
    first_title = PREFIX + "delayed-one"
    first_gate = delayed(s, wait_for, first, first_title)
    second, second_point = capture(s, 12, 0.98, 0.98)
    second_title = PREFIX + "delayed-two"
    second_gate = delayed(s, wait_for, second, second_title)
    capture(s, 13)
    second_gate.touch()
    check_placement(s, wait_for, second_title, 12, second_point, border)
    s.close()
    first_gate.touch()
    check_placement(s, wait_for, first_title, 11, first_point, border)
    assert not s.status()["live"]
    s.check("reverse-order delayed launches retain workspace and cursor point through overview dismissal")

    # Rename before consumption and then again after consumption: neither
    # transition changes the existing captured workspace object.
    s.setup("dwindle", 0, 1)
    app = activation_client(s)
    open_overview(s, wait_for)
    token, point = capture(s, 13)
    s.ctl("dispatch", "renameworkspace", "13 hs-launch-renamed")
    activation = s.request("consume " + token)
    assert activation
    s.ctl("dispatch", "renameworkspace", "13 hs-launch-renamed-again")
    title = PREFIX + "renamed"
    assert s.protocol(app, f"after {title} {activation}") == "ok"
    window = check_placement(s, wait_for, title, 13, point, border)
    assert window["workspace"]["name"] == "hs-launch-renamed-again"
    s.close()
    s.ctl("dispatch", "renameworkspace", "13 13")
    s.check("workspace renames before and after token consumption preserve the captured live destination")

    s.setup("dwindle", 0, 1)
    # Workspace 13 is already empty and persistent in the canonical setup.
    # Make it inactive without relying on asynchronously materializing a new
    # persistent workspace rule immediately before overview collection.
    s.ctl("dispatch", "focusmonitor", s.names[2])
    s.ctl("dispatch", "workspace", "15")
    try:
        open_overview(s, wait_for)
        token, _ = capture(s, 13)
        s.ctl("dispatch", "hyprspace-test:replace-workspace", "13")
        assert s.request("consume " + token) == ""
        s.check("destroyed workspace captures cannot resolve a replacement with identical ID and name")

        time.sleep(0.1)
        token, _ = capture(s, 13)
        title = PREFIX + "replaced-after-consume"
        gate = delayed(s, wait_for, token, title)
        s.ctl("dispatch", "hyprspace-test:replace-workspace", "13")
        s.close()
        s.ctl("dispatch", "focusmonitor", s.names[0])
        s.ctl("dispatch", "workspace", "11")
        gate.touch()
        window = wait_for(lambda: s.windows().get(title))
        assert window["workspace"]["id"] == 11, window
        s.check("delayed mapped windows retain native placement after their captured workspace is replaced")
    finally:
        s.close()

    s.setup("dwindle", 0, 1)
    open_overview(s, wait_for)
    token, point = capture(s, 12, 0.9, 0.9)
    title = PREFIX + "transferred"
    gate = delayed(s, wait_for, token, title)
    source = area(next(m for m in s.data("monitors") if m["name"] == s.names[1]))
    s.ctl("dispatch", "moveworkspacetomonitor", f"12 {s.names[0]}")
    s.ctl("keyword", "monitor", f"{s.names[1]},disable")
    s.await_outputs(2)
    try:
        target_monitor = next(m for m in s.data("monitors") if m["name"] == s.names[0])
        target = area(target_monitor)
        mapped = {
            "x": target["x"] + (point["x"] - source["x"]) * target["w"] / source["w"],
            "y": target["y"] + (point["y"] - source["y"]) * target["h"] / source["h"],
        }
        gate.touch()
        window = check_placement(s, wait_for, title, 12, mapped, border)
        assert window["monitor"] == target_monitor["id"]
        s.check("live workspace identity remaps cursor placement after transfer and original-output removal")
    finally:
        s.close()
        s.ctl("keyword", "monitor", f"{s.names[1]},960x600@60,-1000x-200,1.25,transform,1")
        s.await_outputs(3)


def run(s, wait_for):
    fixture = s.artifact("test-overview.so")
    s.ctl("plugin", "load", str(fixture))
    # Fixture loading schedules a config reload. Let it finish before adding
    # private runtime rules that the native reload would otherwise clear.
    time.sleep(0.15)
    border = s.data("getoption general:border_size")["int"]
    s.ctl("keyword", "windowrule", f"match:title ^{PREFIX}.*$, float on")
    for name in s.names:
        s.ctl("keyword", "monitor", f"{name},addreserved,36,14,12,18")
    try:
        geometry(s, wait_for, border)
        identity(s, wait_for, border)
    finally:
        s.close()
        s.ctl("reload")
        s.await_outputs(3)
        assert s.ctl("configerrors") == ""
        s.ctl("plugin", "unload", str(fixture))
