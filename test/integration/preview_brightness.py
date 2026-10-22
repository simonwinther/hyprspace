"""Photometric minification checks in the runner's private compositor.

Flat swatches alone cannot detect encoded-RGB averaging: fine white strokes
lose light when mixed with a dark background. Compare those patterns and real
font rasterization against native source pixels, with a fixed selected target.
"""

import json
import sys
import time

from PIL import Image, ImageStat

from zoom import inspection, released, wheel


CLIENT = '''import gi,cairo
gi.require_version("Gtk","3.0")
from gi.repository import Gtk
w=Gtk.Window(title="hs-brightness-pattern")
w.set_decorated(False);w.set_default_size(1200,700)
def draw(widget,c):
 width,height=widget.get_allocated_width(),widget.get_allocated_height()
 cw,ch=width/4,height/2
 c.set_operator(cairo.OPERATOR_SOURCE);c.set_source_rgb(.06,.06,.06);c.paint()
 for k,g in enumerate((.06,.25,.5,.75)):
  c.set_source_rgb(g,g,g);c.rectangle(k*cw,0,cw,ch);c.fill()
 c.set_antialias(cairo.ANTIALIAS_NONE);c.set_source_rgb(1,1,1)
 for x in range(int(cw)):
  if x%4==0:c.rectangle(x,ch,1,ch)
 c.fill()
 for y in range(int(ch)):
  for x in range(int(cw)):
   if (x+y)%2==0:c.rectangle(cw+x,ch+y,1,1)
 c.fill()
 c.set_antialias(cairo.ANTIALIAS_DEFAULT)
 c.select_font_face("monospace",cairo.FONT_SLANT_NORMAL,cairo.FONT_WEIGHT_NORMAL);c.set_font_size(10)
 for y in range(int(ch)+15,height-5,13):
  c.move_to(cw*2+8,y);c.show_text("let windows = workspace.visible();")
 c.set_source_rgb(.5,.5,.5);c.rectangle(cw*3+30,ch+30,cw-60,ch-60);c.fill()
 return True
w.connect("draw",draw);w.connect("destroy",lambda _:Gtk.main_quit());w.show_all();Gtk.main()
'''


def samples(path, box, scale):
    pixels = Image.open(path).convert("RGB")
    result = {}
    names = ("dark", "gray25", "gray50", "gray75", "strokes25", "checker50", "text", "graybox")
    for row in range(2):
        for column in range(4):
            left = round((box["x"] + box["w"] * (column + .2) / 4) * scale)
            right = round((box["x"] + box["w"] * (column + .8) / 4) * scale)
            top = round((box["y"] + box["h"] * (row + .2) / 2) * scale)
            bottom = round((box["y"] + box["h"] * (row + .8) / 2) * scale)
            assert 0 <= left < right <= pixels.width and 0 <= top < bottom <= pixels.height, (box, pixels.size)
            patch = pixels.crop((left, top, right, bottom))
            histogram = patch.histogram()
            energy = []
            for channel in range(3):
                total = 0
                for level, count in enumerate(histogram[channel * 256:(channel + 1) * 256]):
                    encoded = level / 255
                    light = encoded / 12.92 if encoded <= .04045 else ((encoded + .055) / 1.055) ** 2.4
                    total += light * count
                energy.append(total / (patch.width * patch.height))
            result[names[row * 4 + column]] = {
                "mean_rgb": [mean / 255 for mean in ImageStat.Stat(patch).mean],
                "mean_linear_luma": sum(value * weight for value, weight in zip(energy, (.2126, .7152, .0722))),
            }
    return result


def run(s, wait_for):
    name, title, scale = s.names[0], "hs-brightness-pattern", 1.2
    report = {"generation": s.snapshot.record, "cases": []}
    s.close()
    for process in s.processes[1:]:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
    s.processes = s.processes[:1]
    wait_for(lambda: not s.windows())
    s.ctl("keyword", "monitor", f"{name},1920x1080@60,0x0,{scale},transform,0")
    wait_for(lambda: (monitor := next(value for value in s.data("monitors") if value["name"] == name))["width"] == 1920 and monitor["scale"] == scale)
    for key, value in (("animations:enabled", "false"), ("decoration:shadow:enabled", "false"),
                       ("decoration:dim_inactive", "false"), ("plugin:hyprspace:overview:bg_dim", "1"),
                       ("plugin:hyprspace:warp_cursor", "false"), ("input:follow_mouse", "0")):
        s.ctl("keyword", key, value)
    for number in (11, 14, 15, 16):
        s.ctl("keyword", "workspace", f"{number},monitor:{name},persistent:true")
    s.ctl("dispatch", "focusmonitor", name)
    s.ctl("dispatch", "workspace", "11")
    client = s.root / "brightness-client.py"
    client.write_text(CLIENT)
    s.spawn(["python3", str(client)])
    window = wait_for(lambda: s.windows().get(title))
    address = window["address"]
    s.ctl("dispatch", "setfloating", "address:" + address)
    s.ctl("dispatch", "resizewindowpixel", "exact 1200 700,address:" + address)
    s.ctl("dispatch", "movewindowpixel", "exact 200 120,address:" + address)
    wait_for(lambda: s.windows()[title]["size"] == [1200, 700])

    def shot(stem, box=None):
        path = s.root / (stem + ".png")
        s.run("grim", "-s", str(scale), "-o", name, str(path))
        return samples(path, box or s.preview(title), scale)

    try:
        for opacity, blur in ((1, "off"), (.85, "off"), (.85, "live"), (.85, "cached")):
            s.close()
            s.ctl("keyword", "decoration:active_opacity", str(opacity))
            s.ctl("keyword", "decoration:inactive_opacity", str(opacity if opacity == 1 else .8))
            s.ctl("keyword", "decoration:blur:enabled", str(blur != "off").lower())
            s.ctl("keyword", "decoration:blur:new_optimizations", str(blur == "cached").lower())
            s.ctl("keyword", "decoration:blur:xray", str(blur == "cached").lower())
            s.ctl("dispatch", "focuswindow", "address:" + address)
            time.sleep(.15)
            window = s.windows()[title]
            source_box = dict(zip(("x", "y", "w", "h"), [*window["at"], *window["size"]]))
            stem = f"brightness-opacity{opacity}-blur{blur}"
            native = shot(stem + "-native", source_box)
            case = {"opacity": opacity, "blur": blur, "native": native, "modes": {}}
            for mode in ("linear", "area", "sharp"):
                s.close()
                s.ctl("keyword", "plugin:hyprspace:overview:preview_filter", mode)
                s.ctl("dispatch", "hyprspace:overview", "on")
                wait_for(lambda: s.status()["live"] and s.status()["keyboard_owned"] and s.status()["resources"]["captures"]["bytes"] > 0)
                if mode != "linear":
                    wait_for(lambda: s.status()["resources"]["preview_filter"]["bytes"] > 0)
                time.sleep(.15)
                target = s.status()["target"]["workspace"]
                grid = shot(stem + f"-{mode}-grid")
                s.move((5, 5))
                s.move(s.preview_point(title, .5, .5))
                assert s.status()["target"]["workspace"] == target
                s.key(44, 1)
                inspection(s, wait_for, 1)
                wheel(s, -3)
                inspection(s, wait_for, 1.15 ** 3)
                time.sleep(.15)
                assert s.preview(title)["w"] >= window["size"][0] * .95
                zoomed = shot(stem + f"-{mode}-source-bypass")
                s.key(44, 0)
                released(s, wait_for)
                resources = s.status()["resources"]["preview_filter"]
                assert resources["failures"] == 0, resources
                case["modes"][mode] = {"grid": grid, "zoom": zoomed}
            report["cases"].append(case)
            (s.root / "preview-brightness.json").write_text(json.dumps(report, indent=2))
            linear = case["modes"]["linear"]
            for mode in ("area", "sharp"):
                value = case["modes"][mode]
                for flat in ("dark", "gray25", "gray50", "gray75"):
                    assert max(abs(a - b) for a, b in zip(value["grid"][flat]["mean_rgb"], linear["grid"][flat]["mean_rgb"])) <= 3 / 255, (mode, flat, case)
                for pattern in ("strokes25", "checker50", "text"):
                    actual = value["grid"][pattern]["mean_linear_luma"]
                    reference = native[pattern]["mean_linear_luma"]
                    assert .75 <= actual / reference <= 1.25, (mode, pattern, actual, reference)
                assert value["grid"]["text"]["mean_linear_luma"] >= .9 * value["zoom"]["text"]["mean_linear_luma"], (mode, case)
                for patch in native:
                    assert max(abs(a - b) for a, b in zip(value["zoom"][patch]["mean_rgb"], linear["zoom"][patch]["mean_rgb"])) <= 3 / 255, (mode, patch, case)
            s.check(f"opacity {opacity}, blur {blur}: minified strokes/checker/text preserve native light; flat colors and enlarged source agree across filters")
        s.close()
        s.ctl("keyword", "plugin:hyprspace:overview:preview_filter", "sharp")
        s.ctl("dispatch", "hyprspace:overview", "on")
        wait_for(lambda: s.status()["live"] and s.status()["keyboard_owned"])
        s.move((5, 5))
        s.move(s.preview_point(title, .5, .5))
        selected_workspace = s.status()["target"]["workspace"]
        selected = shot("brightness-same-window-selected")
        s.key(106, 1)  # Right selects the next card without focusing a desktop window.
        s.key(106, 0)
        wait_for(lambda: s.status()["target"]["workspace"] != selected_workspace)
        assert s.data("activewindow")["address"] == address
        unselected = shot("brightness-same-window-unselected")
        for patch in selected:
            assert max(abs(a - b) for a, b in zip(selected[patch]["mean_rgb"], unselected[patch]["mean_rgb"])) <= 2 / 255, (patch, selected, unselected)
        report["selection_consistency"] = {"selected": selected, "unselected": unselected}
        s.check("changing the selected workspace adds no extra fade to the same window's thumbnail")
        s.close()
        resources = s.status()["resources"]
        assert resources["captures"]["bytes"] == resources["textures"]["bytes"] == resources["preview_filter"]["bytes"] == 0, resources
        assert not s.status()["keyboard_owned"] and not s.status()["cursor_owned"]
        s.check("brightness comparisons preserve target selection and release capture/filter/UI resources and input ownership")
    finally:
        original_error = sys.exc_info()[1]
        (s.root / "preview-brightness.json").write_text(json.dumps(report, indent=2))
        try:
            if s.status()["zoom"]["held"]:
                s.key(44, 0)
            s.close()
        except Exception:
            if original_error is None:
                raise
