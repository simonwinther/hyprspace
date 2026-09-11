"""Empty layout and rapid toggle regressions in the private compositor."""

import time


def lifecycle(s, wait_for):
    s.setup("dwindle", 0, 1)
    fixture = s.plugin.parent / "test-overview.so"
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
