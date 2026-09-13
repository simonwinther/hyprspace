"""Bound title/width churn, capture failures and overlay resource lifetimes."""

import json


def limits(s, wait_for):
    s.setup("dwindle", 0, 1)
    fixture = s.artifact("test-overview.so")
    s.ctl("plugin", "load", str(fixture))
    try:
        measurements = {}
        for mode in ("churn", "allocation"):
            response = s.run("hyprctl", "dispatch", "hyprspace-test:resources", mode)
            (s.root / f"resources-{mode}.txt").write_text(response)
            measurements[mode] = json.loads(response)
            s.check(f"resource {mode}: bounds, reuse, eviction and failure recovery")
        (s.root / "resources.json").write_text(json.dumps(measurements, indent=2))
    finally:
        s.ctl("plugin", "unload", str(fixture))
    for _ in range(3):
        s.ctl("dispatch", "hyprspace:overview", "on")
        wait_for(lambda: s.status()["resources"]["captures"]["bytes"] > 0)
        s.close()
        wait_for(lambda: s.status()["resources"]["captures"]["bytes"] == 0)
        wait_for(lambda: s.status()["resources"]["textures"]["bytes"] == 0)
        assert s.status()["resources"]["textures"]["entries"] == 0
        s.ctl("dispatch", "hyprspace:switch")
        wait_for(lambda: s.status()["resources"]["textures"]["bytes"] > 0)
        s.close()
        wait_for(lambda: s.status()["resources"]["textures"]["bytes"] == 0)
    s.check("overview and switcher close/reopen release all capture and cached GPU bytes")
    for overlay in ("overview", "switch"):
        s.ctl("dispatch", f"hyprspace:{overlay}")
        wait_for(lambda: s.status()["resources"]["textures"]["bytes"] > 0)
        s.ctl("plugin", "unload", str(s.plugin))
        s.run("wtype", "z")
        s.ctl("plugin", "load", str(s.plugin))
        s.ctl("reload")
        assert not s.status()["live"]
        s.check(f"unloading an open {overlay} safely releases textures retained by native render passes")
