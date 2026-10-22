#!/usr/bin/env python3
"""Compare real overview filters without opening or capturing the host desktop.

The same live windows, native pixel output and grid geometry are reused for all
filters. Screenshots are unmodified; detail PNGs only crop native pixels. Timing
is compositor CPU submission, not GPU completion or physical presentation.
"""

import argparse
import html
import json
from pathlib import Path
import platform
import signal
import sys
import time

from run import REPO, Suite, wait_for
from performance import percentile


SOURCE = '''// A stable Rust editing workload: no generated timestamps or animations.
use std::collections::{BTreeMap, VecDeque};

#[derive(Clone, Debug, PartialEq)]
struct Workspace {
    number: u32,
    name: String,
    windows: Vec<String>,
}

#[derive(Default)]
struct Space {
    name: String,
    workspaces: BTreeMap<u32, Workspace>,
    history: VecDeque<u32>,
}

impl Space {
    fn visit(&mut self, number: u32) -> Option<&Workspace> {
        let workspace = self.workspaces.get(&number)?;
        self.history.retain(|previous| *previous != number);
        self.history.push_front(number);
        self.history.truncate(64);
        Some(workspace)
    }

    fn occupied(&self) -> usize {
        self.workspaces.values()
            .filter(|workspace| !workspace.windows.is_empty())
            .count()
    }
}

fn main() {
    let mut school = Space::default();
    school.name = "Independent spaces".into();
    println!("{}: {} occupied workspaces", school.name, school.occupied());
}
'''


def article(path, dark=False):
    background, foreground, secondary, panel = (
        ("#151923", "#d9e1ef", "#99a5b8", "#212837") if dark
        else ("#f7f5f0", "#252b35", "#637082", "#ebe9e3")
    )
    path.write_text(f'''<!doctype html><html><head><meta charset="utf-8">
<title>hs-preview-browser-{'dark' if dark else 'light'}</title>
<style>
*{{box-sizing:border-box}}body{{margin:0;background:{background};color:{foreground};font:18px/1.6 sans-serif}}
nav{{padding:18px 7%;border-bottom:1px solid {secondary};font-size:15px;letter-spacing:.03em}}
main{{max-width:1000px;margin:35px auto;padding:0 35px}}h1{{font-size:39px;line-height:1.15;margin:12px 0 20px}}
h2{{font-size:25px;margin:30px 0 8px}}p{{margin:12px 0}}.muted{{color:{secondary};font-size:15px}}
pre{{padding:22px;background:{panel};border-radius:10px;font:15px/1.65 monospace;overflow:hidden}}
.graph{{height:135px;display:flex;align-items:end;gap:18px;padding:15px 35px;background:{panel};border-radius:10px}}
.graph i{{background:#82a8e3;display:block;flex:1}}strong{{font-weight:650}}
</style></head><body><nav>FIELD NOTES &nbsp; / &nbsp; INTERFACE DESIGN &nbsp; / &nbsp; OCTOBER</nav>
<main><div class="muted">DESIGN SYSTEMS &nbsp; · &nbsp; 8 MIN READ</div>
<h1>Make room for focused work</h1>
<p>A workspace should be easy to recognize at a glance. Titles, application shapes,
and the arrangement of windows provide context before any text becomes readable.</p>
<h2>Independent spaces, familiar workspaces</h2>
<p>Each space keeps its own window groups and monitor assignments. Switch context
when your task changes, then continue using the same keyboard navigation.</p>
<pre><span style="color:#c087ca">fn</span> select_workspace(number: <span style="color:#8aadf4">u32</span>) {{
    history.record(current_workspace());
    workspace.activate(number);
    monitors.restore_layout();
}}</pre><h2>Preserve the useful details</h2>
<p><strong>Readable headings and stable edges</strong> matter more than a false impression
of sharpness. Avoid bright halos, noisy text, and patterns that change when a card moves.</p>
<div class="graph"><i style="height:35%"></i><i style="height:65%"></i><i style="height:47%"></i>
<i style="height:89%"></i><i style="height:72%"></i><i style="height:96%"></i></div>
<p class="muted">A static article, code sample and chart, rendered by the browser at native output scale.</p>
</main></body></html>''')


def launch(suite, workspace, command, title=None):
    suite.ctl("dispatch", "focusmonitor", suite.names[0])
    suite.ctl("dispatch", "workspace", str(workspace))
    before = {window["address"] for window in suite.data("clients")}
    suite.spawn(command)

    def mapped():
        candidates = [window for window in suite.data("clients") if window["address"] not in before]
        return next((window for window in candidates if title is None or window["title"].startswith(title)), None)

    window = wait_for(mapped, timeout=25)
    suite.ctl("dispatch", "movetoworkspacesilent", f'{workspace},address:{window["address"]}')
    return window


def configure_output(suite, scale):
    suite.ctl("keyword", "monitor", f"{suite.names[0]},1920x1080@60,0x0,{scale},transform,0")
    suite.ctl("keyword", "monitor", f"{suite.names[1]},960x600@60,2200x0,1,transform,0")
    suite.ctl("keyword", "monitor", f"{suite.names[2]},960x600@60,3300x0,1,transform,0")
    wait_for(lambda: (monitor := next(m for m in suite.data("monitors") if m["name"] == suite.names[0]))
             and monitor["width"] == 1920 and monitor["height"] == 1080 and abs(monitor["scale"] - scale) < .001)


def setup_content(suite):
    suite.ctl("keyword", "animations:enabled", "false")
    suite.ctl("keyword", "general:gaps_in", "4")
    suite.ctl("keyword", "general:gaps_out", "8")
    suite.ctl("keyword", "decoration:blur:enabled", "false")
    suite.ctl("keyword", "plugin:hyprspace:warp_cursor", "false")
    suite.ctl("keyword", "plugin:hyprspace:follow_mouse", "false")
    suite.ctl("keyword", "input:follow_mouse", "0")
    source = suite.root / "workspace.rs"
    source.write_text(SOURCE * 3)
    second = suite.root / "capture.rs"
    second.write_text((REPO / "src/PreviewStyle.hpp").read_text())
    for workspace in range(11, 16):
        suite.ctl("keyword", "workspace", f"{workspace},monitor:{suite.names[0]},persistent:true")

    def terminal(workspace, title, path, light=False, size=13):
        return launch(suite, workspace, [
            "ghostty", "--config-default-files=false", f"--font-size={size}",
            f"--background={'f7f5f0' if light else '151923'}", f"--foreground={'252b35' if light else 'd9e1ef'}",
            "--cursor-style-blink=false", "--shell-integration=none", "--confirm-close-surface=false",
            f"--title={title}", "-e", "nvim", "-n", "-i", "NONE", "-u", "NONE", "+set number noshowmode noruler laststatus=2",
            "+syntax on", "+set background=" + ("light" if light else "dark"),
            "+set guicursor=a:hor1-blinkon0", "+set statusline=PREVIEW\\ FILTER\\ COMPARISON", str(path),
        ], title)

    first = terminal(11, "hs-preview-dark-code", source)
    terminal(11, "hs-preview-dark-layout", second)
    terminal(13, "hs-preview-light-code", source, light=True, size=16)
    terminal(15, "hs-preview-small-code", source, size=12)
    terminal(15, "hs-preview-small-layout", second, size=12)

    profile = suite.root / "firefox-profile"
    profile.mkdir(exist_ok=True)
    (profile / "user.js").write_text(
        'user_pref("browser.aboutwelcome.enabled", false);\n'
        'user_pref("browser.shell.checkDefaultBrowser", false);\n'
        'user_pref("browser.startup.homepage_override.mstone", "ignore");\n'
        'user_pref("toolkit.cosmeticAnimations.enabled", false);\n'
        'user_pref("browser.tabs.warnOnClose", false);\n')
    suite.env["MOZ_ENABLE_WAYLAND"] = "1"
    suite.env["MOZ_DBUS_REMOTE"] = "1"
    for workspace, dark in ((12, False), (14, True)):
        page = suite.root / f"article-{'dark' if dark else 'light'}.html"
        article(page, dark)
        launch(suite, workspace, ["firefox", "--profile", str(profile), "--new-window", page.as_uri()],
               f"hs-preview-browser-{'dark' if dark else 'light'}")
    suite.ctl("dispatch", "focuswindow", "address:" + first["address"])
    suite.move((8, 8))  # stay in the primary margin, away from tiles and tooltips
    suite.ctl("dispatch", "focuswindow", "address:" + first["address"])
    time.sleep(1)
    suite.check("stable native terminal and browser fixtures occupy five identical workspace cards")
    return first


def canvas(state, monitor):
    view = next(view for view in state["views"] if view["monitor"] == monitor)
    return sorted((tile["workspace"], tile["window"], *(round(tile[key], 5) for key in ("x", "y", "w", "h")))
                  for tile in view["tiles"])


def write_comparison(destination, variants, scales):
    choices = []
    for scale in scales:
        for label, _ in variants:
            filename = f"overview-{scale:g}-{label}.png"
            choices.append(f'<button data-src="{html.escape(filename)}">{html.escape(label)} · scale {scale:g}</button>')
    (destination / "comparison.html").write_text('''<!doctype html><meta charset="utf-8"><title>Native preview comparison</title>
<style>body{margin:0;background:#17191d;color:#eee;font:15px system-ui}header{position:sticky;top:0;background:#25272d;padding:12px;z-index:1}
button{padding:8px 14px;margin:4px;border:1px solid #60666f;background:#343841;color:#eee;border-radius:5px;cursor:pointer}
button.active{background:#3666a4}p{margin:5px}#stage{overflow:auto}img{display:block;max-width:none}</style>
<header><p>Unmodified screenshots, sized for one image pixel per device pixel at your current browser scale. Use the buttons to A/B without changing scroll position.</p>
''' + ''.join(choices) + '''</header><div id="stage"><img id="frame"></div><script>
const frame=document.querySelector('#frame'),buttons=[...document.querySelectorAll('button')];
function nativePixels(){frame.style.width=frame.naturalWidth/window.devicePixelRatio+'px'}
frame.onload=nativePixels;window.addEventListener('resize',nativePixels);
for(const button of buttons)button.onclick=()=>{
buttons.forEach(b=>b.classList.toggle('active',b===button));frame.src=button.dataset.src;frame.alt=button.textContent};buttons[0].click();
</script>''')


def animated_cycles(suite, args, report, first):
    """Exercise real opening/closing frames, including fractional card sizes."""
    configure_output(suite, args.scales[0])
    suite.ctl("keyword", "animations:enabled", "true")
    suite.ctl("keyword", "animation", "windowsMove,1,4,default")
    suite.ctl("dispatch", "focuswindow", "address:" + first["address"])
    output_id = next(m["id"] for m in suite.data("monitors") if m["name"] == suite.names[0])
    before = suite.geometry()
    report["animated"] = []
    for label, value in args.variants:
        suite.ctl("keyword", "plugin:hyprspace:overview:preview_filter", value)
        suite.ctl("keyword", "plugin:hyprspace:diagnostics", "false")
        suite.ctl("keyword", "plugin:hyprspace:diagnostics", "true")
        samples, peak_bytes, peak_filter_bytes = {}, 0, 0
        before_filtering = suite.status()["resources"]["preview_filter"]

        def collect():
            nonlocal peak_bytes, peak_filter_bytes
            state = suite.status()
            for frame in state["diagnostics"]["samples"]:
                if frame["monitor"] == output_id:
                    samples[frame["time_ms"]] = frame
            captures, filters = state["resources"]["captures"], state["resources"]["preview_filter"]
            assert captures["bytes"] <= captures["limit_bytes"] and filters["failures"] == 0, state["resources"]
            peak_bytes = max(peak_bytes, captures["bytes"])
            peak_filter_bytes = max(peak_filter_bytes, filters["bytes"])
            return state

        for cycle in range(args.animation_cycles):
            cycle_allocations = suite.status()["resources"]["preview_filter"]["allocations"]
            suite.ctl("dispatch", "hyprspace:overview", "on")
            wait_for(lambda: suite.status()["live"])
            deadline = time.monotonic() + .6
            while time.monotonic() < deadline:
                collect()
                time.sleep(.02)
            suite.ctl("dispatch", "hyprspace:close")
            wait_for(lambda: not collect()["views"])
            closed = suite.status()
            assert closed["resources"]["captures"]["bytes"] == closed["resources"]["textures"]["bytes"] == 0
            assert closed["resources"]["preview_filter"]["entries"] == closed["resources"]["preview_filter"]["bytes"] == 0
            allocations = closed["resources"]["preview_filter"]["allocations"] - cycle_allocations
            assert allocations <= 2 * len(suite.data("clients")), (label, cycle, allocations)
            if value == "linear":
                assert allocations == 0, allocations
            assert not closed["keyboard_owned"] and not closed["cursor_owned"]
            assert suite.geometry() == before, (label, cycle, before, suite.geometry())
            assert suite.data("activewindow")["address"] == first["address"]
        frames = list(samples.values())
        after_filtering = suite.status()["resources"]["preview_filter"]
        assert len(frames) >= 8 * args.animation_cycles, len(frames)
        assert after_filtering["animated_bypasses"] > before_filtering["animated_bypasses"], after_filtering
        report["animated"].append({
            "label": label, "value": value, "cycles": args.animation_cycles, "samples": len(frames),
            "render_p95_ms": percentile([frame["render_ms"] for frame in frames]),
            "capture_p95_ms": percentile([frame["capture_ms"] for frame in frames]),
            "frame_interval_p95_ms": percentile([frame["interval_ms"] for frame in frames if frame["interval_ms"] > 0]),
            "maximum_resident_capture_bytes": peak_bytes, "maximum_resident_filter_bytes": peak_filter_bytes,
            "filter_allocations": after_filtering["allocations"] - before_filtering["allocations"],
            "animated_bypasses": after_filtering["animated_bypasses"] - before_filtering["animated_bypasses"],
        })
        (args.output.resolve() / "comparison.json").write_text(json.dumps(report, indent=2))
        suite.check(f"{label}: {args.animation_cycles} animated open/close cycles preserve desktop geometry/focus and release every filter buffer")
    suite.ctl("keyword", "animations:enabled", "false")


def run(args):
    from PIL import Image

    destination = args.output.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    suite = Suite(group="preview-clarity", build_dir=args.build)
    error = None
    report = {
        "measurement": "Native unmodified output screenshots; CPU render submission diagnostics exclude GPU completion and physical presentation",
        "platform": platform.platform(), "generation": suite.snapshot.record,
        "scales": args.scales, "variants": [], "content": "Static local Firefox article in dark/light; actual Ghostty/Neovim source in dark/light",
    }
    try:
        configure_output(suite, args.scales[0])
        first = setup_content(suite)
        suite.ctl("dismissnotify")
        for scale in args.scales:
            configure_output(suite, scale)
            for workspace in range(11, 16):
                suite.ctl("dispatch", "workspace", str(workspace))
                wait_for(lambda: suite.data("activeworkspace")["id"] == workspace)
                time.sleep(.15)
                suite.run("grim", "-s", str(scale), "-o", suite.names[0],
                          str(destination / f"native-{scale:g}-ws{workspace}.png"))
            suite.ctl("dispatch", "focuswindow", "address:" + first["address"])
            baseline_geometry = None
            for label, value in args.variants:
                suite.ctl("keyword", "plugin:hyprspace:overview:preview_filter", value)
                suite.ctl("keyword", "plugin:hyprspace:diagnostics", "false")
                suite.ctl("keyword", "plugin:hyprspace:diagnostics", "true")
                suite.ctl("dispatch", "hyprspace:overview", "on")
                wait_for(lambda: suite.status()["live"] and suite.status()["resources"]["captures"]["bytes"] > 0)
                initial = suite.status()["diagnostics"]["frames"]
                wait_for(lambda: suite.status()["diagnostics"]["frames"] >= initial + 12)
                state = suite.status()
                geometry = canvas(state, suite.names[0])
                if baseline_geometry is None:
                    baseline_geometry = geometry
                else:
                    assert geometry == baseline_geometry, (label, geometry, baseline_geometry)
                assert state["resources"]["captures"]["downsampled"] == 0, state["resources"]
                filtering = state["resources"]["preview_filter"]
                assert filtering["mode"] == value and filtering["failures"] == 0, filtering
                if value != "linear":
                    assert filtering["entries"] > 0 and filtering["bytes"] > 0, filtering
                assert state["resources"]["captures"]["bytes"] <= state["resources"]["captures"]["limit_bytes"]
                filename = f"overview-{scale:g}-{label}.png"
                suite.ctl("dismissnotify")
                suite.run("grim", "-s", str(scale), "-o", suite.names[0], str(destination / filename))
                with Image.open(destination / filename) as frame:
                    assert frame.size == (1920, 1080), frame.size
                    for workspace in (11, 12, 13, 14, 15):
                        tile = next(tile for view in state["views"] if view["monitor"] == suite.names[0]
                                    for tile in view["tiles"] if tile["workspace"] == workspace and tile["window"] == "0x0")
                        bounds = tuple(round(tile[key] * scale) for key in ("x", "y", "w", "h"))
                        x, y, width, height = bounds
                        frame.crop((x, y, x + width, y + height)).save(destination / f"detail-{scale:g}-{label}-ws{workspace}.png")
                samples = {}
                output_id = next(m["id"] for m in suite.data("monitors") if m["name"] == suite.names[0])
                for step in range(args.samples):
                    suite.key(15, 1)
                    suite.key(15, 0)
                    time.sleep(.02)
                    for frame in suite.status()["diagnostics"]["samples"]:
                        if frame["monitor"] == output_id:
                            samples[frame["time_ms"]] = frame
                frames = list(samples.values())
                inputs = [frame["input_ms"] for frame in frames if frame["input_ms"] >= 0]
                assert len(inputs) >= min(20, args.samples // 2), len(inputs)
                report["variants"].append({
                    "scale": scale, "label": label, "value": value, "screenshot": filename,
                    "samples": len(frames), "input_samples": len(inputs),
                    "input_p95_ms": percentile(inputs), "render_p95_ms": percentile([f["render_ms"] for f in frames]),
                    "capture_p95_ms": percentile([f["capture_ms"] for f in frames]),
                    "frame_interval_p95_ms": percentile([f["interval_ms"] for f in frames if f["interval_ms"] > 0]),
                    "captures": state["resources"]["captures"], "filter": filtering, "geometry": geometry,
                })
                (destination / "comparison.json").write_text(json.dumps(report, indent=2))
                suite.close()
                closed = suite.status()
                assert closed["resources"]["captures"]["bytes"] == closed["resources"]["textures"]["bytes"] == 0
                assert closed["resources"]["preview_filter"]["entries"] == closed["resources"]["preview_filter"]["bytes"] == 0
                assert not closed["keyboard_owned"] and not closed["cursor_owned"]
                suite.check(f"{label}, scale {scale:g}: same native geometry, full-resolution screenshots, bounded timing and zero resources after close")
        animated_cycles(suite, args, report, first)
        report["outputs"] = suite.data("monitors")
        assert suite.ctl("configerrors") == ""
        write_comparison(destination, args.variants, args.scales)
    except BaseException as caught:
        error = caught
        raise
    finally:
        report["runtime"] = str(suite.root)
        report["error"] = repr(error) if error else None
        (destination / "comparison.json").write_text(json.dumps(report, indent=2))
        suite.finish()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=REPO / "build")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--scales", type=float, nargs="+", default=[1.2])
    parser.add_argument("--variants", nargs="+", default=["linear=linear", "area=area", "sharp=sharp", "linear-sharp=linear-sharp"])
    parser.add_argument("--samples", type=int, default=60)
    parser.add_argument("--animation-cycles", type=int, default=4)
    args = parser.parse_args()
    args.variants = [variant.split("=", 1) for variant in args.variants]
    if not args.scales or any(scale not in (1, 1.2, 1.25, 1.5, 2) for scale in args.scales):
        parser.error("use ordinary tested output scales: 1, 1.2, 1.25, 1.5, 2")
    if any(len(variant) != 2 or not variant[0].replace("-", "").isalnum() for variant in args.variants):
        parser.error("variants are filename-safe label=config-value pairs")
    if args.samples < 20:
        parser.error("at least 20 samples are required")
    if not 1 <= args.animation_cycles <= 20:
        parser.error("animation cycles must be between 1 and 20")
    for signum in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, lambda sig, frame: sys.exit(128 + sig))
    run(args)
