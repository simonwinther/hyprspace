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
        space_workloads(s, wait_for, report)
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
                assert s.status()["live"] and not s.status()["window_board"]["search_focused"]
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


def space_workloads(s, wait_for, report):
    """Hold active content fixed while inactive spaces grow, then time switches.

    Switch timestamps come from the compositor's steady clock. Python polling
    and hyprctl startup are excluded, as are animation completion/presentation.
    """
    initial = s.status().get("spaces", {})
    if not initial.get("enabled"):
        report["spaces"] = {"enabled": False}
        return
    original = initial["active"]
    created = []
    inactive_addresses = set()
    measurements = {"enabled": True, "inactive_growth": [], "warm_switches": [],
                    "measurement": "space dispatcher entry to first target-generation RENDER_POST on every awake output; excludes presentation"}
    report["spaces"] = measurements

    def dispatch(action, **values):
        return s.ctl("dispatch", "hyprspace:space", json.dumps({"action": action, **values}))

    def active_space():
        return s.status()["spaces"]["active"]

    def capture_fixed_active(inactive_count, active_titles):
        s.ctl("keyword", "plugin:hyprspace:diagnostics", "false")
        s.ctl("keyword", "plugin:hyprspace:diagnostics", "true")
        s.ctl("dispatch", "hyprspace:overview", "on")
        wait_for(lambda: len(s.status()["views"]) == 3)
        # Finish opening and membership reconciliation before checking the
        # steady state. Content capture remains live throughout the sample.
        start = {v["monitor"]: v["layout"]["frames"] for v in s.status()["views"]}
        wait_for(lambda: all(v["layout"]["frames"] >= start[v["monitor"]] + 6
                             for v in s.status()["views"]))
        steady = s.status()
        builds = steady["spaces"]["membership_rebuilds"]
        expected = {}
        for title in active_titles:
            monitor = s.windows()[title]["monitor"]
            expected[monitor] = expected.get(monitor, 0) + 1
        cutoff = max(frame["time_ms"] for frame in steady["diagnostics"]["samples"])
        frames = {}

        def collect():
            state = s.status()
            for frame in state["diagnostics"]["samples"]:
                if frame["time_ms"] > cutoff and frame["space"] == original:
                    frames[(frame["monitor"], frame["time_ms"])] = frame
            assert state["spaces"]["membership_rebuilds"] == builds, state["spaces"]
            inactive = [w for w in state["windows"] if w["address"] in inactive_addresses]
            assert len(inactive) == len(inactive_addresses) and all(w["suspended"] for w in inactive), inactive
            return state if len(frames) >= 90 else None

        state = wait_for(collect)
        for frame in frames.values():
            assert frame["captures"] == expected.get(frame["monitor"], 0), (expected, frame)
        result = {"inactive_spaces": inactive_count, "inactive_windows": len(inactive_addresses),
                  "active_windows": len(active_titles), "samples": len(frames),
                  "capture_bytes": state["resources"]["captures"]["bytes"],
                  "capture_pixels_per_monitor": {
                      str(monitor): max(f["pixels"] for f in frames.values() if f["monitor"] == monitor)
                      for monitor in expected},
                  "prepare_p95_ms": percentile([f["prepare_ms"] for f in frames.values()]),
                  "render_p95_ms": percentile([f["render_ms"] for f in frames.values()]),
                  "capture_p95_ms": percentile([f["capture_ms"] for f in frames.values()]),
                  "membership_rebuilds_during_sample": state["spaces"]["membership_rebuilds"] - builds}
        measurements["inactive_growth"].append(result)
        if inactive_count:
            baseline = measurements["inactive_growth"][0]
            assert result["capture_bytes"] == baseline["capture_bytes"], (baseline, result)
            assert result["capture_pixels_per_monitor"] == baseline["capture_pixels_per_monitor"], (baseline, result)
        (s.root / "performance.json").write_text(json.dumps(report, indent=2))


        s.close()

    try:
        s.setup("dwindle", 0, 1)
        extra = [f"hs-space-active-{i}" for i in range(9)]
        s.spawn(["python3", str(s.client), *extra])
        wait_for(lambda: len(s.windows()) == 12)
        active_titles = list(s.windows())
        for i, title in enumerate(active_titles):
            s.ctl("dispatch", "movetoworkspacesilent", f'{11 + i % 3},address:{s.windows()[title]["address"]}')
        capture_fixed_active(0, active_titles)
        for index in range(4):
            name = f"performance inactive {index}"
            dispatch("create", name=name)
            owner = wait_for(lambda: next((v for v in s.status()["spaces"]["spaces"] if v["name"] == name), None))
            created.append(owner["id"])
            wait_for(lambda: active_space() == owner["id"])
            for monitor, number in zip(s.names, (21, 22, 23)):
                s.ctl("dispatch", "focusmonitor", monitor)
                s.ctl("dispatch", "workspace", str(number))
            titles = [f"hs-space-inactive-{index}-{i}" for i in range(6)]
            s.spawn(["python3", str(s.client), *titles])
            wait_for(lambda: all(title in s.windows() for title in titles))
            for i, title in enumerate(titles):
                window = s.windows()[title]
                s.ctl("dispatch", "movetoworkspacesilent", f'{21 + i % 3},address:{window["address"]}')
                inactive_addresses.add(window["address"])
        dispatch("switch", id=original)
        wait_for(lambda: active_space() == original)
        capture_fixed_active(4, active_titles)
        s.check("inactive-space growth leaves active captures unchanged, inactive clients suspended and steady membership cached")

        s.ctl("dispatch", "hyprspace:overview", "on")
        wait_for(lambda: len(s.status()["views"]) == 3)
        outputs = s.data("monitors")
        monitor_ids = {str(m["id"]) for m in outputs if m.get("dpmsStatus", True) and m.get("mirrorOf") in (None, "", "none")}
        budget = max(2000 / m["refreshRate"] for m in outputs)

        def switch_and_wait(destination):
            before = s.status()["spaces"]["last_switch"]["generation"]
            dispatch("switch", id=destination)

            def rendered():
                status = s.status()
                value = status["spaces"]["last_switch"]
                return value if (value["space"] == destination and value["generation"] > before
                                 and monitor_ids.issubset(value["renders"])) else None

            value = wait_for(rendered)
            elapsed = max(value["renders"][monitor] for monitor in monitor_ids) - value["start_ms"]
            assert elapsed >= 0, value
            return {"space": destination, "generation": value["generation"], "render_ms": elapsed,
                    "monitor_render_ms": {monitor: value["renders"][monitor] - value["start_ms"]
                                          for monitor in monitor_ids}}

        # Prime client redraws and shared CPU/icon caches before timing repeated
        # switches. Each transition still allocates current-space captures.
        switch_and_wait(created[0])
        switch_and_wait(original)
        for i in range(20):
            measurements["warm_switches"].append(switch_and_wait(created[0] if i % 2 == 0 else original))
        measurements["warm_switch_p95_ms"] = percentile([v["render_ms"] for v in measurements["warm_switches"]])
        measurements["two_refresh_intervals_ms"] = budget
        for i in range(100):
            switch_and_wait(created[0] if i % 2 == 0 else original)
            state = s.status()
            assert state["resources"]["captures"]["bytes"] <= state["resources"]["captures"]["limit_bytes"]
            assert state["resources"]["textures"]["bytes"] <= state["resources"]["textures"]["limit_bytes"]
        s.close()
        state = s.status()
        assert state["resources"]["captures"]["bytes"] == state["resources"]["textures"]["bytes"] == 0
        assert not state["cursor_owned"] and not state["keyboard_owned"] and not state["pending_resize"]
        measurements["cleanup_switches"] = 100
        s.check("100 space switches retain bounded GPU resources and closing releases input and GPU ownership")
        # Collect cleanup evidence even when the independent timing gate fails.
        assert measurements["warm_switch_p95_ms"] <= budget, measurements
        s.check(f'space-switch render p95 {measurements["warm_switch_p95_ms"]:.2f}ms within two refresh intervals')
    finally:
        s.close()
        if active_space() != original:
            dispatch("switch", id=original)
            wait_for(lambda: active_space() == original)
        for owner in created:
            dispatch("delete", id=owner, target=original)
        s.ctl("keyword", "plugin:hyprspace:diagnostics", "false")
        (s.root / "performance.json").write_text(json.dumps(report, indent=2))

def compare_reports(baseline, candidate):
    """Compare matching machine/output workloads without treating stress as a gate."""
    for key in ("cpu", "platform", "measurement"):
        if baseline[key] != candidate[key]:
            raise ValueError(f"Cannot compare different {key}: {baseline[key]!r} / {candidate[key]!r}")
    if baseline["compositor"]["abiHash"] != candidate["compositor"]["abiHash"]:
        raise ValueError("Cannot compare different compositor ABIs")
    geometry = ("name", "width", "height", "refreshRate", "scale", "transform", "x", "y", "reserved")
    outputs = lambda report: sorted(tuple(json.dumps(m.get(k), sort_keys=True) for k in geometry) for m in report["outputs"])
    if outputs(baseline) != outputs(candidate):
        raise ValueError("Cannot compare different output geometry or refresh rates")
    result = {"cpu": candidate["cpu"], "compositor_abi": candidate["compositor"]["abiHash"], "comparisons": [], "accepted": True,
              "note": "A single paired run records regressions; repeat on an otherwise idle machine before attributing a timing difference."}
    for category in ("workloads", "window_boards"):
        before = {v["windows"]: v for v in baseline[category]}
        after = {v["windows"]: v for v in candidate[category]}
        if before.keys() != after.keys():
            raise ValueError(f"Cannot compare different {category} window counts")
        for count, prior in before.items():
            current = after[count]
            comparison = {"view": category, "windows": count, "stress": current["stress"], "metrics": {}}
            for metric in ("input_p95_ms", "prepare_p95_ms", "render_p95_ms", "capture_p95_ms"):
                previous, present = prior[metric], current[metric]
                limit = previous + max(previous * .1, .5)
                gated = not current["stress"] and metric in ("prepare_p95_ms", "render_p95_ms")
                accepted = not gated or present <= limit
                comparison["metrics"][metric] = {"baseline": previous, "candidate": present, "delta_ms": present - previous,
                                                   "delta_percent": (present / previous - 1) * 100 if previous else None,
                                                   "limit": limit if gated else None, "accepted": accepted}
                result["accepted"] = result["accepted"] and accepted
            result["comparisons"].append(comparison)
    return result


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Compare paired Hyprspace performance reports on matching hardware and outputs")
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        comparison = compare_reports(json.loads(args.baseline.read_text()), json.loads(args.candidate.read_text()))
    except (ValueError, KeyError, OSError) as error:
        parser.error(str(error))
    result = json.dumps(comparison, indent=2) + "\n"
    if args.output:
        args.output.write_text(result)
    else:
        print(result, end="")
    raise SystemExit(0 if comparison["accepted"] else 1)
