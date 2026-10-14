"""Close empty workspace cards without dismissing the overview or losing windows."""

from empty_workspaces import button, native, opened, press, reset, selected, view
from launch_regressions import delayed
from zoom import center, tile

Q, X, Z = 16, 45, 44


def select(s, wait_for, workspace, monitor):
    s.move(center(tile(s, workspace, s.names[monitor])))
    wait_for(lambda: s.status()["target"]["workspace"] == workspace)


def gone(s, wait_for, workspace, monitor):
    wait_for(lambda: not any(t["workspace"] == workspace for t in view(s, monitor)["tiles"]))
    assert s.status()["live"] and s.status()["keyboard_owned"]


def active(s, monitor):
    return next(m for m in s.data("monitors") if m["name"] == s.names[monitor])["activeWorkspace"]["id"]


def nearest(s, workspace, monitor):
    cells = {t["workspace"]: t for t in view(s, monitor)["tiles"] if t["window"] == "0x0"}
    x, y = center(cells[workspace])
    return min((key for key in cells if key != workspace),
               key=lambda key: ((center(cells[key])[0] - x) ** 2 + (center(cells[key])[1] - y) ** 2, key))


def prepare(s, wait_for):
    s.move(center(button(s, 0)))
    press(s)
    return selected(s, wait_for, 0)


def run(s, wait_for):
    try:
        reset(s, wait_for)
        opened(s, wait_for)
        workspace = prepare(s, wait_for)
        before = native(s)
        geometry = s.geometry()
        cursor = s.data("cursorpos")
        press(s, Q)
        gone(s, wait_for, workspace, 0)
        wait_for(lambda: not any(w["id"] == workspace for w in s.data("workspaces")))
        assert native(s) == before and s.geometry() == geometry and s.data("cursorpos") == cursor
        assert s.status()["target"]["workspace"] == 11
        s.check("Q closes a prepared empty workspace, keeps the overview open, and preserves the active desktop, cursor and windows")

        reset(s, wait_for)
        opened(s, wait_for)
        select(s, wait_for, 11, 0)
        before = native(s)
        geometry = s.geometry()
        press(s, Q)
        assert s.geometry() == geometry and native(s) == before
        assert any(t["workspace"] == 11 for t in view(s, 0)["tiles"])
        assert "only empty workspaces" in s.ctl("dispatch", "hyprspace:closeworkspace")
        s.check("Q and the dispatcher protect populated workspaces")

        select(s, wait_for, 13, 2)
        before = native(s)
        press(s, Q)
        assert native(s) == before and active(s, 2) == 13
        assert "last workspace" in s.ctl("dispatch", "hyprspace:closeworkspace")
        assert any(t["workspace"] == 13 for t in view(s, 2)["tiles"])
        s.check("the last normal workspace on a monitor is protected even when other outputs have workspaces")

        reset(s, wait_for)
        for workspace in (31, 32):
            s.ctl("keyword", "workspace", f"{workspace},monitor:{s.names[0]},persistent:true")
        wait_for(lambda: {31, 32} <= {w["id"] for w in s.data("workspaces")})
        s.ctl("dispatch", "focusmonitor", s.names[0])
        s.ctl("dispatch", "workspace", "31")
        opened(s, wait_for)
        select(s, wait_for, 31, 0)
        replacement = nearest(s, 31, 0)
        geometry = s.geometry()
        s.run("grim", "-s", "1", "-c", "-o", s.names[0], str(s.root / "close-workspace-before.png"))
        press(s, Q)
        gone(s, wait_for, 31, 0)
        wait_for(lambda: not any(w["id"] == 31 for w in s.data("workspaces")))
        assert active(s, 0) == replacement and s.status()["target"]["workspace"] == replacement
        assert s.geometry() == geometry
        s.run("grim", "-s", "1", "-c", "-o", s.names[0], str(s.root / "close-workspace-after.png"))
        s.check("closing an active empty workspace activates its nearest remaining tile and keeps the overview open")

        assert replacement == 32
        press(s, Q)
        gone(s, wait_for, 32, 0)
        assert active(s, 0) == 11 and s.data("activewindow")["address"] == s.windows()["hs-A"]["address"]
        assert s.geometry() == geometry
        s.close()
        assert active(s, 0) == 11 and not any(w["id"] in (31, 32) for w in s.data("workspaces"))
        s.check("switching to a populated neighbor restores valid native focus; dismissing the overview does not recreate closed workspaces")

        reset(s, wait_for)
        for workspace in (31, 32):
            s.ctl("keyword", "workspace", f"{workspace},monitor:{s.names[0]},persistent:true")
        opened(s, wait_for)
        select(s, wait_for, 31, 0)
        before = active(s, 0)
        s.key(Q, 1)
        gone(s, wait_for, 31, 0)
        select(s, wait_for, 32, 0)
        for _ in range(3):
            s.key(Q, 1)
        assert any(t["workspace"] == 32 for t in view(s, 0)["tiles"]) and active(s, 0) == before
        s.key(Q, 0)
        press(s, Q)
        gone(s, wait_for, 32, 0)
        s.check("holding Q closes once; a new physical press can close the next empty workspace")

        reset(s, wait_for)
        opened(s, wait_for)
        workspace = prepare(s, wait_for)
        s.key(Z, 1)
        wait_for(lambda: s.status()["zoom"]["held"])
        press(s, Q)
        gone(s, wait_for, workspace, 0)
        s.key(Z, 0)
        assert not s.status()["zoom"]["held"]
        s.check("Q can close an inspected empty workspace without closing the overview or leaving zoom held")

        reset(s, wait_for)
        opened(s, wait_for)
        s.ctl("keyword", "plugin:hyprspace:follow_mouse", "false")
        workspace = prepare(s, wait_for)
        press(s, 107)  # End
        assert s.status()["target"]["workspace"] == 11
        press(s, 102)  # Home
        assert s.status()["target"]["workspace"] == workspace
        s.move(center(tile(s, 13, s.names[2])))
        before = native(s)
        press(s, Q)
        gone(s, wait_for, workspace, 0)
        assert native(s) == before and active(s, 2) == 13
        s.check("Q acts on the keyboard selection with follow_mouse disabled, independently of the pointer's output")

        reset(s, wait_for)
        s.ctl("keyword", "workspace", f"name:hs-close-named,monitor:{s.names[0]},persistent:true")
        workspace = wait_for(lambda: next((w["id"] for w in s.data("workspaces") if w["name"] == "hs-close-named"), None))
        opened(s, wait_for)
        select(s, wait_for, workspace, 0)
        press(s, Q)
        gone(s, wait_for, workspace, 0)
        wait_for(lambda: not any(w["id"] == workspace for w in s.data("workspaces")))
        s.check("empty named normal workspaces can be closed")

        reset(s, wait_for)
        s.ctl("dispatch", "togglespecialworkspace", "hs-close-special")
        workspace = next(w["id"] for w in s.data("workspaces") if w["name"] == "special:hs-close-special")
        opened(s, wait_for)
        select(s, wait_for, workspace, 0)
        before = native(s)
        press(s, Q)
        assert native(s) == before and any(t["workspace"] == workspace for t in view(s, 0)["tiles"])
        assert "normal workspace" in s.ctl("dispatch", "hyprspace:closeworkspace")
        s.check("Q protects special workspaces")

        reset(s, wait_for)
        opened(s, wait_for)
        workspace = prepare(s, wait_for)
        token = s.request("capture")
        gate = delayed(s, wait_for, token, "hs-close-delayed")
        if not s.status()["live"]:
            opened(s, wait_for)
            assert prepare(s, wait_for) == workspace
        press(s, Q)
        gone(s, wait_for, workspace, 0)
        gate.touch()
        wait_for(lambda: s.windows().get("hs-close-delayed", {}).get("workspace", {}).get("id") == workspace)
        wait_for(lambda: any(t["workspace"] == workspace for t in view(s, 0)["tiles"]))
        assert s.status()["layout_targets_unique"]
        s.check("closing a card preserves a pending launch's identity and shows its workspace again when the window arrives")

        reset(s, wait_for)
        opened(s, wait_for)
        workspace = prepare(s, wait_for)
        s.ctl("dispatch", "hyprspace:help", "on")
        help_entry = next(e for e in s.status()["help"]["entries"] if e["id"] == "close-workspace")
        assert help_entry["keys"] == "Q" and help_entry["title"] == "Close an empty workspace"
        s.ctl("keyword", "plugin:hyprspace:overview:close_workspace_key", "x")
        wait_for(lambda: next(e for e in s.status()["help"]["entries"] if e["id"] == "close-workspace")["keys"] == "X")
        s.ctl("dispatch", "hyprspace:help", "off")
        press(s, Q)
        assert any(t["workspace"] == workspace for t in view(s, 0)["tiles"])
        press(s, X)
        gone(s, wait_for, workspace, 0)
        workspace = prepare(s, wait_for)
        s.ctl("keyword", "plugin:hyprspace:overview:close_workspace_key", "")
        press(s, Q)
        press(s, X)
        assert any(t["workspace"] == workspace for t in view(s, 0)["tiles"])
        assert s.ctl("dispatch", "hyprspace:closeworkspace") == "ok"
        gone(s, wait_for, workspace, 0)
        s.check("the close shortcut appears in help, can be remapped or disabled, and leaves the dispatcher available")

        workspace = prepare(s, wait_for)
        s.ctl("keyword", "plugin:hyprspace:overview:close_workspace_key", "q")
        s.ctl("keyword", "plugin:hyprspace:overview:zoom_key", "q")
        s.key(Q, 1)
        wait_for(lambda: s.status()["zoom"]["held"])
        s.key(Q, 0)
        assert any(t["workspace"] == workspace for t in view(s, 0)["tiles"])
        s.check("an existing Q zoom binding takes precedence over the new close shortcut")
    finally:
        s.key(Q, 0)
        s.key(Z, 0)
        s.close()
        s.ctl("reload")
