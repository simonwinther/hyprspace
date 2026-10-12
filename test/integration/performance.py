"""Opt-in responsiveness measurements and repeated resource cleanup.

Input latency ends at compositor RENDER_POST, before output commit. GPU
completion, physical presentation and subjective animation quality require
separate measurements.
"""

import json
import math
from pathlib import Path
import platform
import time


def percentile(values, fraction=.95):
    return sorted(values)[max(0, math.ceil(len(values) * fraction) - 1)] if values else None


def run(s, wait_for):
    report = {
        "measurement": "input event to compositor RENDER_POST (CPU render submission); excludes GPU completion and display presentation",
        "platform": platform.platform(),
        "cpu": next((line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
                     if line.startswith("model name")), platform.machine()),
        "compositor": s.data("version"),
        "outputs": s.data("monitors"),
        "workloads": [],
    }
    try:
        for count in (3, 12, 36):
            s.setup("dwindle", 0, 1)
            if count > 3:
                extra = [f"hs-perf-{i}" for i in range(count - 3)]
                s.spawn(["python3", str(s.client), *extra])
                wait_for(lambda: len(s.windows()) == count, timeout=15)
            for i, window in enumerate(s.windows().values()):
                s.ctl("dispatch", "movetoworkspacesilent", f'{11 + i % 3},address:{window["address"]}')
            s.ctl("keyword", "plugin:hyprspace:diagnostics", "false")
            s.ctl("keyword", "plugin:hyprspace:diagnostics", "true")
            s.ctl("dispatch", "hyprspace:overview", "on")
            wait_for(lambda: len(s.status()["views"]) == 3)
            time.sleep(.3)
            captured = {}
            for step in range(80):
                # Cross output boundaries as well as window hit regions.
                monitor = report["outputs"][step % 3]
                width = monitor["width"] / monitor["scale"]
                height = monitor["height"] / monitor["scale"]
                if monitor["transform"] % 2:
                    width, height = height, width
                s.move((monitor["x"] + width * (.25 if step % 2 else .75),
                        monitor["y"] + height * (.35 if step % 4 < 2 else .65)))
                if step % 8 == 0:
                    s.scroll()
                for frame in s.status()["diagnostics"]["samples"]:
                    captured[(frame["monitor"], frame["time_ms"])] = frame
            frames = list(captured.values())
            inputs = [frame["input_ms"] for frame in frames if frame["input_ms"] >= 0]
            assert len(inputs) >= 30, len(inputs)
            ordinary_budget = max(2000 / output["refreshRate"] for output in report["outputs"])
            workload = {
                "windows": count, "stress": count == 36, "samples": len(frames), "input_samples": len(inputs),
                "input_p95_ms": percentile(inputs), "prepare_p95_ms": percentile([f["prepare_ms"] for f in frames]),
                "render_p95_ms": percentile([f["render_ms"] for f in frames]),
                "capture_p95_ms": percentile([f["capture_ms"] for f in frames]),
                "two_refresh_intervals_ms": ordinary_budget,
            }
            report["workloads"].append(workload)
            (s.root / "performance.json").write_text(json.dumps(report, indent=2))
            if count != 36:
                assert workload["input_p95_ms"] <= ordinary_budget, workload
            assert len(s.status()["diagnostics"]["samples"]) <= 64
            s.check(f'{count} windows: input/render p95 {workload["input_p95_ms"]:.2f}ms'
                    + (" (stress measurement)" if count == 36 else " within two refresh intervals"))
            s.close()

        window_boards(s, wait_for, report)

        s.setup("dwindle", 0, 1)
        for _ in range(100):
            s.ctl("dispatch", "hyprspace:overview", "on")
            wait_for(lambda: s.status()["resources"]["captures"]["bytes"] > 0)
            s.close()
            status = s.status()
            assert status["resources"]["captures"]["bytes"] == 0
            assert status["resources"]["textures"]["bytes"] == 0
            assert not status["cursor_owned"] and not status["keyboard_owned"]
            assert not status["pending_resize"]
        report["cleanup_cycles"] = 100
        s.ctl("keyword", "plugin:hyprspace:diagnostics", "false")
        wait_for(lambda: not s.status()["diagnostics"]["enabled"])
        assert not s.status()["diagnostics"]["samples"]
        s.check("100 open/close cycles release GPU resources, input ownership and pending placement")
    finally:
        s.close()
        s.ctl("keyword", "plugin:hyprspace:diagnostics", "false")
        (s.root / "performance.json").write_text(json.dumps(report, indent=2))


def window_boards(s, wait_for, report):
    """Measure board input against its display output, including remote selections."""
    report["window_boards"] = []
    for count in (3, 12, 36):
        s.setup("dwindle", 0, 1)
        if count > 3:
            s.spawn(["python3", str(s.client), *[f"hs-board-perf-{i}" for i in range(count - 3)]])
            wait_for(lambda: len(s.windows()) == count, timeout=15)
        for i, window in enumerate(s.windows().values()):
            s.ctl("dispatch", "movetoworkspacesilent", f'{11 + i % 3},address:{window["address"]}')
        s.ctl("keyword", "plugin:hyprspace:diagnostics", "false")
        s.ctl("keyword", "plugin:hyprspace:diagnostics", "true")
        s.ctl("dispatch", "hyprspace:overview", "on")
        s.ctl("dispatch", "hyprspace:windowview", "app")
        wait_for(lambda: s.status()["window_board"]["count"] == count)
        time.sleep(.3)
        captured = {}
        host = next(output for output in report["outputs"] if output["name"] == s.status()["window_board"]["monitor"])
        for step in range(80):
            s.key(15, 1)
            s.key(15, 0)
            if step % 10 == 0:
                mode = ("flat", "app", "workspace", "monitor")[(step // 10) % 4]
                s.ctl("dispatch", "hyprspace:windowview", mode)
            time.sleep(.018)
            for frame in s.status()["diagnostics"]["samples"]:
                if frame["monitor"] == host["id"]:
                    captured[frame["time_ms"]] = frame
        frames = list(captured.values())
        inputs = [frame["input_ms"] for frame in frames if frame["input_ms"] >= 0]
        assert len(inputs) >= 30, len(inputs)
        budget = 2000 / host["refreshRate"]
        workload = {
            "windows": count, "stress": count == 36, "samples": len(frames), "input_samples": len(inputs),
            "input_p95_ms": percentile(inputs), "prepare_p95_ms": percentile([f["prepare_ms"] for f in frames]),
            "render_p95_ms": percentile([f["render_ms"] for f in frames]),
            "capture_p95_ms": percentile([f["capture_ms"] for f in frames]),
            "two_refresh_intervals_ms": budget,
        }
        report["window_boards"].append(workload)
        (s.root / "performance.json").write_text(json.dumps(report, indent=2))
        if count != 36:
            assert workload["input_p95_ms"] <= budget, workload
        s.check(f'{count} board windows: input/render p95 {workload["input_p95_ms"]:.2f}ms'
                + (" (stress measurement)" if count == 36 else " within two refresh intervals"))
        if count == 36:
            collapsed = next((window for window in s.windows().values() if min(window["size"]) < 1), None)
            if collapsed:
                s.run("wtype", "-k", "slash", collapsed["title"])
                wait_for(lambda: s.status()["window_board"]["count"] == 1)
                assert s.status()["window_board"]["selected_window"] == collapsed["address"]
                preview = s.status()["window_board"]["previews"][0]
                assert all(math.isfinite(preview[part]) for part in ("x", "y", "w", "h"))
                s.run("wtype", "-k", "Return")
                wait_for(lambda: not s.status()["views"])
                assert s.data("activewindow")["address"] == collapsed["address"]
                s.check("collapsed native windows retain finite board previews and can be searched and focused")
        s.close()

    s.setup("dwindle", 0, 1)
    for step in range(100):
        s.ctl("dispatch", "hyprspace:overview", "on")
        s.ctl("dispatch", "hyprspace:windowview", ("flat", "app", "workspace", "monitor")[step % 4])
        wait_for(lambda: s.status()["resources"]["captures"]["bytes"] > 0)
        s.close()
        state = s.status()
        assert state["resources"]["captures"]["bytes"] == state["resources"]["textures"]["bytes"] == 0
        assert not state["cursor_owned"] and not state["keyboard_owned"]
    report["window_board_cleanup_cycles"] = 100
    s.check("100 board open/close cycles release captures, text/icon textures and input ownership")
