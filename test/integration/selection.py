"""Pointer hit testing and command/commit selection in both follow_mouse modes."""

import time


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
