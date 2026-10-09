"""Pointer hit testing and command/commit selection in both follow_mouse modes."""

import time


def shortcuts(s, wait_for):
    from zoom import inspection, opened

    def tap(code):
        s.key(code, 1)
        s.key(code, 0)

    s.setup("dwindle", 0, 1)
    for workspace in (21, 22):
        s.ctl("keyword", "workspace", f"{workspace},monitor:{s.names[0]},persistent:true")
    s.ctl("dispatch", "movetoworkspacesilent", "21,address:" + s.windows()["hs-B"]["address"])
    try:
        for mode in ("grid", "fitted", "magnified"):
            opened(s, wait_for)
            if mode != "grid":
                s.key(44, 1)  # Z
                inspection(s, wait_for, 1)
            if mode == "magnified":
                tap(13)  # '='
                inspection(s, wait_for, 1.15)
            for shift in (42, 54):  # Left and right Shift.
                tap(15)  # Tab
                assert s.status()["target"]["workspace"] == 21
                s.key(shift, 1)
                tap(15)
                s.key(shift, 0)
                assert s.status()["target"]["workspace"] == 11
                if mode != "grid":
                    assert s.status()["zoom"]["held"] and s.status()["zoom"]["workspace"] == 11
            tap(15)
            if mode == "magnified":
                inspection(s, wait_for, 1)
                tap(13)
                inspection(s, wait_for, 1.15)
            assert s.status()["target"]["workspace"] == 21
            log = s.root / "input.log"
            log.write_text("")
            s.key(31, 1)  # S commits while Z is still held.
            wait_for(lambda: not s.status()["views"])
            s.key(31, 1)  # A repeat and late releases must remain captured.
            s.key(31, 0)
            if mode != "grid":
                s.key(44, 0)
            assert s.data("activeworkspace")["id"] == 21
            assert s.data("activewindow")["address"] == s.windows()["hs-B"]["address"]
            assert not s.status()["zoom"]["held"] and s.status()["modifiers"] == 0
            time.sleep(0.1)
            assert " 115" not in log.read_text()
            s.run("wtype", "s")
            wait_for(lambda: " 115" in log.read_text())
            s.check(f"{mode}: both Shift+Tab chords go backward, S commits the selection and fresh typing resumes")
    finally:
        for code in (15, 31, 42, 44, 54):
            s.key(code, 0)
        s.close()
        for workspace in (21, 22):
            s.ctl("keyword", "workspace", f"{workspace},persistent:false")


def consistency(s, wait_for):
    try:
        for follow in (False, True):
            s.setup("dwindle", 0, 1)
            s.ctl("keyword", "workspace", f"21,monitor:{s.names[0]},persistent:true")
            s.ctl("dispatch", "movetoworkspacesilent", "21,address:" + s.windows()["hs-B"]["address"])
            s.ctl("dispatch", "focuswindow", "address:" + s.windows()["hs-A"]["address"])
            s.ctl("keyword", "plugin:hyprspace:follow_mouse", "true")
            s.ctl("dispatch", "hyprspace:overview", "on")
            s.move(s.preview_point("hs-A"))
            s.ctl("keyword", "plugin:hyprspace:follow_mouse", str(follow).lower())
            s.move(s.preview_point("hs-B"))
            time.sleep(0.15)  # also exercise refresh of a stationary pointer
            chosen = "hs-B" if follow else "hs-A"
            address = s.windows()[chosen]["address"]
            assert s.status()["target"]["window"] == address, s.status()["target"]
            s.ctl("dispatch", "togglefloating")
            assert s.windows()[chosen]["floating"]
            assert not s.windows()["hs-A" if follow else "hs-B"]["floating"]
            assert s.status()["target"]["window"] == address
            s.run("wtype", "-k", "Return")
            wait_for(lambda: not s.status()["views"])
            assert s.data("activewindow")["address"] == address
            s.check(f"follow_mouse={follow}: hover, native commands and Enter use one selected identity")

            s.ctl("dispatch", "hyprspace:overview", "on")
            s.run("wtype", "-k", "Home")
            assert s.status()["target"]["workspace"] == 11
            s.move(s.preview_point("hs-C"))
            expected = 12 if follow else 11
            assert s.status()["target"]["workspace"] == expected
            s.run("wtype", "-k", "Return")
            wait_for(lambda: not s.status()["views"])
            assert s.data("activeworkspace")["id"] == expected
            s.check(f"follow_mouse={follow}: pointer motion across outputs respects keyboard selection")

            s.ctl("dispatch", "focuswindow", "address:" + s.windows()["hs-A"]["address"])
            s.ctl("dispatch", "hyprspace:overview", "on")
            if not follow:
                assert s.status()["target"]["window"] == s.windows()["hs-A"]["address"]
            s.move(s.preview_point("hs-A"))
            s.key(125, 1)
            try:
                s.button(1)
                assert s.status()["dragging"]
                s.move(s.preview_point("hs-B"))
                s.button(0)
            finally:
                s.button(0)
                s.key(125, 0)
            wait_for(lambda: s.windows()["hs-A"]["workspace"]["id"] == 21)
            assert not s.status()["dragging"] and s.status()["layout_targets_unique"]
            s.close()
            s.check(f"follow_mouse={follow}: drag destinations continue to follow pointer hit testing")
    finally:
        s.ctl("keyword", "plugin:hyprspace:follow_mouse", "true")
        s.ctl("keyword", "workspace", "21,persistent:false")
        s.close()
