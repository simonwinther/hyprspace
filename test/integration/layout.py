"""Measure layout computations in stable and changing overview states."""

import json


def counts(s, wait_for):
    def counters():
        return next(view["layout"] for view in s.status()["views"] if view["monitor"] == s.names[0])

    def sample(frames=120):
        before = counters()
        after = wait_for(lambda: (value if (value := counters())["frames"] >= before["frames"] + frames else None))
        return {key: after[key] - before[key] for key in before}

    s.setup("dwindle", 0, 0)
    s.ctl("dispatch", "fullscreen", "0")
    s.ctl("dispatch", "hyprspace:overview", "on")
    wait_for(lambda: counters()["frames"] >= 10)
    measured = {"steady": sample()}
    s.move(s.preview_point("hs-A"))
    s.key(125, 1)
    try:
        s.button(1)
        assert s.status()["dragging"]
        s.move(s.preview_point("hs-B"))
        # Start the measured interval after entering the fixed-column drag layout.
        sample(3)
        measured["dragging"] = sample()
        s.run("wtype", "-k", "Escape")
    finally:
        s.button(0)
        s.key(125, 0)
    s.ctl("keyword", "animations:enabled", "true")
    s.ctl("keyword", "animation", "windowsMove,1,20,default")
    try:
        s.ctl("dispatch", "hyprspace:overview", "off")
        measured["closing"] = sample(15)
        wait_for(lambda: not s.status()["views"])
    finally:
        s.ctl("keyword", "animations:enabled", "false")
        s.close()
    (s.root / "layout-counts.json").write_text(json.dumps(measured, indent=2))
    print("LAYOUT COUNTS", json.dumps(measured, sort_keys=True), flush=True)
    return measured


def reuse(s, wait_for):
    for phase, measured in counts(s, wait_for).items():
        assert measured["window_updates"] == measured["frames"], measured
        assert measured["window_layouts"] == measured["spread_layouts"] == measured["grid_layouts"] == 0, measured
        s.check(f"{phase}: one window geometry refresh per frame with no unchanged layout solves")

    s.setup("dwindle", 0, 0)
    s.ctl("dispatch", "hyprspace:overview", "on")
    s.move(s.preview_point("hs-A"))

    def view():
        return next(view for view in s.status()["views"] if view["monitor"] == s.names[0])

    def updated(before, kind="window_layouts"):
        wait_for(lambda: view()["layout"][kind] > before["layout"][kind])

    def tile(workspace):
        return next(tile for tile in view()["tiles"] if tile["workspace"] == workspace and tile["window"] == "0x0")

    try:
        before = view()
        s.ctl("dispatch", "fullscreen", "0")
        updated(before)
        previews = [tile for tile in view()["tiles"] if tile["window"] != "0x0"]
        assert len(previews) == 3
        for index, a in enumerate(previews):
            for b in previews[index + 1:]:
                assert min(a["x"] + a["w"], b["x"] + b["w"]) <= max(a["x"], b["x"]) or min(a["y"] + a["h"], b["y"] + b["h"]) <= max(a["y"], b["y"])
        s.check("fullscreen changes invalidate the layout and expose non-overlapping previews")

        address = s.windows()["hs-B"]["address"]
        before = view()
        s.ctl("dispatch", "setfloating", "address:" + address)
        s.ctl("dispatch", "resizewindowpixel", f"exact 350 180,address:{address}")
        updated(before)
        wait_for(lambda: abs(s.preview("hs-B")["w"] / s.preview("hs-B")["h"] - 350 / 180) < 0.001)
        s.check("changed window geometry updates fullscreen spread aspect ratios")

        s.move(s.preview_point("hs-A"))
        before = view()
        s.ctl("dispatch", "fullscreen", "0")
        updated(before)
        wait_for(lambda: s.windows()["hs-A"]["fullscreen"] == 0)
        s.check("leaving fullscreen invalidates the spread layout")

        before = view()
        client = s.client
        s.spawn(["python3", str(client), "hs-layout-D"])
        wait_for(lambda: "hs-layout-D" in s.windows())
        updated(before)
        wait_for(lambda: len([tile for tile in view()["tiles"] if tile["window"] != "0x0"]) == 4)
        before = view()
        s.ctl("dispatch", "closewindow", "address:" + s.windows()["hs-layout-D"]["address"])
        wait_for(lambda: "hs-layout-D" not in s.windows())
        updated(before)
        wait_for(lambda: len([tile for tile in view()["tiles"] if tile["window"] != "0x0"]) == 3)
        s.check("mapping and closing clients refresh membership without stale preview slots")

        before = view()
        s.ctl("keyword", "workspace", f"21,monitor:{s.names[0]},persistent:true")
        s.ctl("dispatch", "movetoworkspacesilent", "21,address:" + s.windows()["hs-C"]["address"])
        updated(before, "grid_layouts")
        wait_for(lambda: s.preview("hs-C")["workspace"] == 21)
        assert {tile["workspace"] for tile in view()["tiles"] if tile["window"] == "0x0"} == {11, 21}
        s.check("workspace membership changes rebuild the grid and retain correct window identities")

        before, bounds = view(), tile(11)
        s.ctl("keyword", "monitor", f"{s.names[0]},addreserved,36,14,12,18")
        updated(before, "grid_layouts")
        updated(before)
        assert tile(11) != bounds
        s.check("output usable geometry invalidates both workspace and window layouts")

        before, bounds = view(), tile(11)
        s.ctl("keyword", "plugin:hyprspace:overview:workspace_labels", "false")
        s.ctl("keyword", "plugin:hyprspace:overview:gap", "40")
        updated(before, "grid_layouts")
        assert tile(11) != bounds
        s.check("label and spacing configuration invalidates the workspace grid")
    finally:
        s.ctl("keyword", "monitor", f"{s.names[0]},addreserved,0,0,0,0")
        s.ctl("keyword", "plugin:hyprspace:overview:workspace_labels", "true")
        s.ctl("keyword", "plugin:hyprspace:overview:gap", "28")
        s.ctl("keyword", "workspace", "21,persistent:false")
        s.close()
