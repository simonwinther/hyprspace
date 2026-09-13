"""Runtime contracts for retained options and overview/switcher scope."""

import json


def contracts(s, wait_for):
    defaults = {"overview:band_gap": 28, "overview:all_workspaces": 1,
                "overview:all_monitors": 1, "switcher:current_workspace_only": 1}
    for name, expected in defaults.items():
        value = json.loads(s.ctl("-j", "getoption", "plugin:hyprspace:" + name))
        assert value["int"] == expected, (name, value)
    s.check("configuration registers the documented overview and switcher defaults")

    def option(name, value):
        s.ctl("keyword", "plugin:hyprspace:" + name, str(value).lower())

    def focus(title):
        s.ctl("dispatch", "focuswindow", "address:" + s.windows()[title]["address"])
        s.move(s.point(s.windows()[title], 0.5, 0.5))

    def views():
        return {v["monitor"]: {t["workspace"] for t in v["tiles"]} for v in s.status()["views"]}

    def geometry():
        return sorted((v["monitor"], t["workspace"], t["window"], t["x"], t["y"], t["w"], t["h"])
                      for v in s.status()["views"] for t in v["tiles"])

    def commit(step):
        s.ctl("dispatch", "hyprspace:switch")
        wait_for(lambda: s.status()["cursor_owned"])
        s.run("wtype", "-k", "Home", *[item for _ in range(step) for item in ("-k", "Tab")], "-k", "Return")
        wait_for(lambda: not s.status()["cursor_owned"])
        return s.data("activewindow")["address"]

    s.setup("dwindle", 0, 1)
    option("follow_mouse", False)
    s.ctl("keyword", "workspace", f"21,monitor:{s.names[0]},persistent:false")
    s.ctl("dispatch", "movetoworkspacesilent", "21,address:" + s.windows()["hs-C"]["address"])
    focus("hs-A")
    s.spawn(["python3", str(s.client), "hs-config-D"])
    wait_for(lambda: "hs-config-D" in s.windows())
    s.ctl("dispatch", "movetoworkspacesilent", "special:config-contract,address:" + s.windows()["hs-config-D"]["address"])
    focus("hs-A")
    special = s.windows()["hs-config-D"]["workspace"]["id"]
    assert special < 0
    try:
        s.ctl("dispatch", "hyprspace:overview", "on")
        expected = {s.names[0]: {11, 21, special}, s.names[1]: {12}, s.names[2]: {13}}
        wait_for(lambda: views() == expected)
        s.check("one global overview session gives each output its own populated, active and persistent workspaces")
        baseline = geometry()
        for name, value in (("overview:band_gap", 0), ("overview:band_gap", 256), ("overview:all_workspaces", False)):
            frame = min(v["layout"]["frames"] for v in s.status()["views"])
            option(name, value)
            wait_for(lambda: min(v["layout"]["frames"] for v in s.status()["views"]) > frame + 2)
            assert geometry() == baseline, (name, views())
        assert s.ctl("configerrors") == ""
        s.check("deprecated band_gap and all_workspaces remain accepted without changing preview geometry or membership")
        s.close()

        option("overview:all_monitors", False)
        focus("hs-A")
        s.ctl("dispatch", "hyprspace:overview", "on")
        wait_for(lambda: views() == {s.names[0]: expected[s.names[0]]})
        b = s.windows()["hs-B"]["address"]
        assert next(w["alpha"] for w in s.status()["windows"] if w["address"] == b) == 1
        s.move(s.point(s.windows()["hs-B"], 0.5, 0.5))
        s.ctl("dispatch", "hyprspace:overview", "on")
        assert set(views()) == {s.names[0]}
        s.ctl("dispatch", "hyprspace:overview")
        wait_for(lambda: not s.status()["views"])
        s.ctl("dispatch", "hyprspace:overview")
        wait_for(lambda: set(views()) == {s.names[1]})
        s.close()
        s.check("all_monitors=false keeps one session: on is idempotent, toggle closes it, reopening follows the pointer")

        focus("hs-A")
        assert commit(2) == s.windows()["hs-A"]["address"]
        focus("hs-B")
        assert commit(1) == b
        monitor = next(m for m in s.data("monitors") if m["name"] == s.names[2])
        s.move((monitor["x"] + 40, monitor["y"] + 40))
        s.ctl("dispatch", "hyprspace:switch")
        assert not s.status()["cursor_owned"]
        s.check("default switcher scope uses the target output's active normal workspace without expanding empty or single-window lists")

        option("switcher:current_workspace_only", False)
        chosen = set()
        for step in range(4):
            focus("hs-A")
            chosen.add(commit(step))
        assert chosen == {w["address"] for w in s.windows().values()}, chosen
        s.check("unrestricted switcher commits windows on other outputs, inactive workspaces and special workspaces")
    finally:
        s.close()
        for name, value in defaults.items():
            option(name, value)
        option("follow_mouse", True)
        s.ctl("keyword", "workspace", "21,persistent:false")
