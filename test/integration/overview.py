"""Initial selection, empty layout and rapid toggles in the private compositor."""

import time


def startup_selection(s, wait_for):
    from zoom import center

    s.setup("dwindle", 0, 1)
    for workspace in (21, 22):
        s.ctl("keyword", "workspace", f"{workspace},monitor:{s.names[0]},persistent:true")
    s.ctl("dispatch", "movetoworkspacesilent", "22,address:" + s.windows()["hs-B"]["address"])
    s.ctl("keyword", "plugin:hyprspace:follow_mouse", "true")
    s.ctl("keyword", "plugin:hyprspace:warp_cursor", "true")
    s.ctl("dispatch", "hyprspace:overview", "on")
    time.sleep(.2)

    def tile(workspace):
        return next(t for view in s.status()["views"] for t in view["tiles"]
                    if t["workspace"] == workspace and t["window"] == "0x0")

    positions = {"empty tile": center(tile(21)), "another tile": center(tile(22)),
                 "another output": center(tile(12))}
    s.close()
    original = s.windows()["hs-A"]["address"]

    def tap(code):
        s.key(code, 1)
        s.key(code, 0)

    def workspaces():
        return {m["name"]: m["activeWorkspace"]["id"] for m in s.data("monitors")}

    try:
        for animated in (False, True):
            s.ctl("keyword", "animations:enabled", str(animated).lower())
            s.ctl("keyword", "animation", "windowsMove,1,3,default")
            for label, position in positions.items():
                for delay in (0, .01, .04, .3):
                    s.ctl("dispatch", "focuswindow", "address:" + original)
                    s.move(position)
                    cursor, active = s.data("cursorpos"), workspaces()
                    s.key(125, 1)
                    try:
                        tap(30)  # Super+A opens without moving the pointer.
                        time.sleep(delay)
                        state = s.status()
                        assert state["live"] and state["target"]["workspace"] == 11, (label, delay, state["target"])
                        assert state["target"]["window"] == original, state["target"]
                        assert s.data("cursorpos") == cursor
                        # Stationary synthetic events and redraws cannot select.
                        s.move(position)
                        assert s.status()["target"]["window"] == original
                        tap(30)  # A second press is an unchanged round trip.
                    finally:
                        s.key(125, 0)
                    wait_for(lambda: not s.status()["views"])
                    assert workspaces() == active and s.data("activewindow")["address"] == original
                    assert s.data("cursorpos") == cursor
                    assert not s.status()["keyboard_owned"] and s.status()["modifiers"] == 0
                s.check(f"stationary cursor on {label}: rapid toggles preserve focus, workspaces and cursor, animations={animated}")

        s.ctl("keyword", "animations:enabled", "false")
        s.ctl("dispatch", "focuswindow", "address:" + original)
        s.move(positions["empty tile"])
        s.ctl("dispatch", "hyprspace:overview", "on")
        time.sleep(.15)
        assert s.status()["target"]["window"] == original
        # An actual movement resumes the usual follow_mouse behavior.
        x, y = positions["empty tile"]
        s.move((x + 2, y))
        assert s.status()["target"]["workspace"] == 21
        s.ctl("dispatch", "hyprspace:overview")
        wait_for(lambda: not s.status()["views"])
        assert s.data("activeworkspace")["id"] == 21
        s.check("real pointer movement selects and commits an empty tile after opening")

        s.ctl("dispatch", "focuswindow", "address:" + original)
        s.move(positions["empty tile"])
        s.ctl("dispatch", "hyprspace:overview", "on")
        time.sleep(.15)
        tap(15)  # Keyboard navigation still commits a deliberate choice.
        assert s.status()["target"]["workspace"] == 21
        s.ctl("dispatch", "hyprspace:overview")
        wait_for(lambda: not s.status()["views"])
        assert s.data("activeworkspace")["id"] == 21
        s.check("keyboard navigation still commits a different selection through the overview toggle")

        s.ctl("dispatch", "focuswindow", "address:" + original)
        s.move(positions["empty tile"])
        s.ctl("dispatch", "hyprspace:overview", "on")
        time.sleep(.15)
        assert s.status()["target"]["window"] == original
        s.button(1)
        s.button(0)
        wait_for(lambda: not s.status()["views"])
        assert s.data("activeworkspace")["id"] == 21
        s.check("an explicit click commits the pointed-at tile without requiring prior motion")

        s.ctl("dispatch", "focuswindow", "address:" + original)
        s.ctl("dispatch", "workspace", "21")  # An empty active workspace has no focused window.
        s.move(positions["another tile"])
        cursor = s.data("cursorpos")
        s.ctl("dispatch", "hyprspace:overview", "on")
        time.sleep(.15)
        assert s.status()["target"]["workspace"] == 21 and s.status()["target"]["window"] == "0x0"
        s.ctl("dispatch", "hyprspace:overview")
        wait_for(lambda: not s.status()["views"])
        assert s.data("activeworkspace")["id"] == 21 and s.data("cursorpos") == cursor
        s.check("an empty active workspace remains selected through an unchanged toggle")

        s.ctl("dispatch", "focuswindow", "address:" + original)
        s.move(positions["another output"])
        s.ctl("keyword", "plugin:hyprspace:overview:all_monitors", "false")
        cursor, active = s.data("cursorpos"), workspaces()
        s.ctl("dispatch", "hyprspace:overview", "on")
        time.sleep(.15)
        state = s.status()
        assert len(state["views"]) == 1 and state["views"][0]["monitor"] == s.names[1]
        assert state["target"]["workspace"] == active[s.names[1]]
        s.ctl("dispatch", "hyprspace:overview")
        wait_for(lambda: not s.status()["views"])
        assert s.data("activewindow")["address"] == original and workspaces() == active
        assert s.data("cursorpos") == cursor
        s.check("a single-output overview seeds that output's active workspace and an unchanged toggle retains original focus")

        s.ctl("keyword", "plugin:hyprspace:overview:all_monitors", "true")
        s.ctl("keyword", "input:follow_mouse", "1")
        s.move(positions["empty tile"])
        cursor, active = s.data("cursorpos"), workspaces()
        assert s.data("activewindow")["address"] == original
        s.ctl("dispatch", "hyprspace:overview", "on")
        time.sleep(.15)
        assert s.status()["target"]["window"] == original
        s.ctl("dispatch", "hyprspace:overview")
        wait_for(lambda: not s.status()["views"])
        assert s.data("activewindow")["address"] == original and workspaces() == active
        assert s.data("cursorpos") == cursor
        s.check("an unchanged toggle preserves focus and cursor with native input:follow_mouse=1")
    finally:
        s.key(125, 0)
        s.close()
        s.ctl("keyword", "animations:enabled", "false")
        s.ctl("keyword", "input:follow_mouse", "0")
        s.ctl("keyword", "plugin:hyprspace:overview:all_monitors", "true")
        for workspace in (21, 22):
            s.ctl("keyword", "workspace", f"{workspace},persistent:false")


def lifecycle(s, wait_for):
    s.setup("dwindle", 0, 1)
    fixture = s.artifact("test-overview.so")
    assert fixture.is_file(), "build integration-fixtures before the overview suite"
    s.ctl("plugin", "load", str(fixture))
    s.ctl("dispatch", "hyprspace:overview", "on")
    wait_for(lambda: len(s.status()["views"]) == 3)
    s.ctl("dispatch", "hyprspace-test:empty-refresh")
    assert s.status()["live"]
    assert all(view["tiles"] for view in s.status()["views"])
    s.check("populated overview survives empty refresh and restores selection on every output")
    s.close()
    s.ctl("plugin", "unload", str(fixture))

    s.ctl("keyword", "bind", "CTRL,A,hyprspace:overview")
    try:
        for animated in (False, True):
            s.ctl("keyword", "animations:enabled", str(animated).lower())
            s.ctl("keyword", "animation", "windowsMove,1,3,default")
            for modifier in (29, 125):  # Control and Super
                for delay in (0, 0.01, 0.04):
                    for _ in range(5):
                        s.key(modifier, 1)
                        try:
                            for transition in range(4):  # open, close, reopen, close
                                s.key(30, 1)
                                s.key(30, 0)
                                assert s.status()["live"] == (transition % 2 == 0)
                                time.sleep(delay)
                        finally:
                            s.key(modifier, 0)
                        wait_for(lambda: not s.status()["views"])
                        state = s.status()
                        assert not state["live"] and not state["keyboard_owned"]
                        assert state["modifiers"] == 0
                        assert all(window["alpha"] == 1 for window in state["windows"])
            s.check(f"120 rapid Control+A and Super+A toggles restore input and visibility, animations={animated}")
    finally:
        s.ctl("keyword", "unbind", "CTRL,A")
        s.ctl("keyword", "animations:enabled", "false")

    log = s.root / "input.log"
    log.write_text("")
    s.run("wtype", "z")
    wait_for(lambda: " 122" in log.read_text())
    s.check("application keyboard input works after rapid overview toggles")
