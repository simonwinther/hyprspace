"""Whole-workspace transfers and gesture ownership on private virtual outputs."""

import json
import subprocess
import time

from protocol import reply
from regressions import center, events, offsets, tile
from resize import area
from zoom import inspection


SUPER, ALT, CTRL, SHIFT, Z = 125, 56, 29, 42, 44
OPTION = "plugin:hyprspace:overview:workspace_drag_modifiers"


def reset(s, wait_for, source=0, destination=1, layout="dwindle"):
    s.close()
    s.ctl("reload")
    assert not s.ctl("configerrors")
    s.setup(layout, source, destination)
    wait_for(lambda: not s.status()["views"])


def opened(s, wait_for, count=3):
    s.ctl("dispatch", "hyprspace:overview", "on")
    wait_for(lambda: len(s.status()["views"]) == count)
    wait_for(lambda: s.status()["keyboard_owned"])
    time.sleep(.15)


def monitor(s, index):
    return next(m for m in s.data("monitors") if m["name"] == s.names[index])


def workspace(s, identity):
    return next(w for w in s.data("workspaces") if w["id"] == identity)


def native(s):
    return {
        m["name"]: (m["activeWorkspace"]["id"], m["specialWorkspace"]["id"], m["focused"])
        for m in s.data("monitors")
    }


def background(s, index):
    work = area(monitor(s, index))
    # Inside the usable output, outside the overview's padded workspace grid.
    return work["x"] + 1, work["y"] + 1


def pickup(s, point, identity, modifiers=(SUPER, ALT), pointer=None):
    s.move(point)
    for code in modifiers:
        s.key(code, 1)
    if pointer:
        pointer.stdin.write("272 1\n")
        pointer.stdin.flush()
        assert reply(pointer) == "ok"
    else:
        s.button(1)
    state = s.status()
    assert state["dragging"] and state["drag"]["kind"] == "workspace", state
    assert state["drag"]["workspace"] == identity and not state["drag"]["resize"], state["drag"]
    assert not state["drag"]["moved"], state["drag"]
    return state["drag"]


def release(s, modifiers=(SUPER, ALT)):
    s.button(0)
    for code in reversed(modifiers):
        s.key(code, 0)


def finish(s, wait_for, identity, destination):
    wait_for(lambda: workspace(s, identity)["monitor"] == s.names[destination])
    state = s.status()
    assert state["live"] and not state["dragging"] and state["layout_targets_unique"], state
    assert state["target"]["workspace"] == identity and state["target"]["window"] == "0x0", state["target"]
    assert any(v["monitor"] == s.names[destination] and any(t["workspace"] == identity for t in v["tiles"]) for v in state["views"])


def transfers(s, wait_for):
    for layout in ("dwindle", "master", "scrolling"):
        for source in range(3):
            for destination in range(3):
                if source == destination:
                    continue
                reset(s, wait_for, source, destination, layout)
                identity = 11 + source
                address = s.windows()["hs-A"]["address"]
                original = workspace(s, identity)
                before = native(s)
                opened(s, wait_for)
                # Alternate pickup over a window and the workspace border. Both
                # must take precedence over the existing native window gesture.
                box = tile(s, identity)
                point = s.preview_point("hs-A") if destination == (source + 1) % 3 else (box["x"] + 2, box["y"] + 2)
                try:
                    drag = pickup(s, point, identity)
                    assert drag["source_monitor"] == s.names[source]
                    assert drag["target_monitor"] == ""
                    destination_point = s.preview_point("hs-B") if destination == (source + 1) % 3 else background(s, destination)
                    s.move(destination_point)
                    drag = s.status()["drag"]
                    assert drag["moved"] and drag["target_monitor"] == s.names[destination], drag
                    assert workspace(s, identity)["monitor"] == s.names[source], "workspace moved before release"
                    cursor = s.data("cursorpos")
                    release(s)
                    finish(s, wait_for, identity, destination)
                    after = native(s)
                    assert after[s.names[destination]] == before[s.names[destination]], (before, after)
                    assert after[s.names[source]][0] != identity and after[s.names[source]][2] == before[s.names[source]][2]
                    assert s.data("cursorpos") == cursor
                    moved = workspace(s, identity)
                    assert (moved["id"], moved["name"]) == (original["id"], original["name"])
                    window = s.windows()["hs-A"]
                    assert window["address"] == address and window["workspace"]["id"] == identity
                    assert window["monitor"] == monitor(s, destination)["id"]
                    assert all(s.windows()[title]["workspace"]["id"] == 11 + destination for title in ("hs-B", "hs-C"))
                finally:
                    release(s)
                    s.close()
        s.check(f"{layout}: all six workspace transfers preserve identity, destination activation, focus and native window membership across scaled and rotated outputs")


def identities_and_windows(s, wait_for):
    for named in (False, True):
        reset(s, wait_for)
        spec = "name:hs-workspace-drag" if named else "21"
        s.ctl("keyword", "workspace", f"{spec},monitor:{s.names[0]},persistent:true,layout:master")
        s.ctl("dispatch", "movetoworkspacesilent", f'{spec},address:{s.windows()["hs-A"]["address"]}')
        identity = s.windows()["hs-A"]["workspace"]["id"]
        before = native(s)
        original_name = workspace(s, identity)["name"]
        opened(s, wait_for)
        pickup(s, s.preview_point("hs-A"), identity)
        # The initiating modifiers can be released before motion and drop.
        s.key(ALT, 0)
        s.key(SUPER, 0)
        assert s.status()["dragging"]
        s.move(background(s, 1))
        release(s)
        finish(s, wait_for, identity, 1)
        assert native(s) == before, (before, native(s))
        assert workspace(s, identity)["name"] == original_name
        s.check(f"inactive {'named' if named else 'numbered'} workspace moves without activating either output and continues after modifier release")

    for prepared in (False, True):
        reset(s, wait_for)
        if not prepared:
            s.ctl("keyword", "workspace", f"21,monitor:{s.names[0]},persistent:true")
        opened(s, wait_for)
        if prepared:
            s.move(center(tile(s, 11)))
            s.ctl("dispatch", "hyprspace:emptyworkspace")
            identity = s.status()["target"]["workspace"]
        else:
            identity = 21
        before = native(s)
        pickup(s, center(tile(s, identity)), identity)
        s.move(background(s, 2))
        release(s)
        finish(s, wait_for, identity, 2)
        time.sleep(.2)  # A stationary refresh must retain the moved empty tile.
        assert workspace(s, identity)["windows"] == 0 and native(s) == before
        assert tile(s, identity)
        s.check(f"{'prepared' if prepared else 'persistent'} empty workspace is retained on its destination until overview dismissal")

    reset(s, wait_for)
    address = s.windows()["hs-A"]["address"]
    s.ctl("dispatch", "setfloating", "address:" + address)
    s.ctl("dispatch", "resizewindowpixel", "exact 300 180,address:" + address)
    s.ctl("dispatch", "movetoworkspacesilent", f'11,address:{s.windows()["hs-C"]["address"]}')
    s.ctl("dispatch", "focuswindow", "address:" + s.windows()["hs-C"]["address"])
    s.ctl("dispatch", "setfloating", "address:" + s.windows()["hs-C"]["address"])
    s.ctl("dispatch", "pin")
    assert s.windows()["hs-C"]["pinned"]
    # Keep an active destination special workspace while moving its regular neighbor.
    s.spawn(["python3", str(s.client), "hs-workspace-special"])
    wait_for(lambda: "hs-workspace-special" in s.windows())
    s.ctl("dispatch", "movetoworkspacesilent", f'special:workspace-drag,address:{s.windows()["hs-workspace-special"]["address"]}')
    s.ctl("dispatch", "focusmonitor", s.names[1])
    s.ctl("dispatch", "togglespecialworkspace", "workspace-drag")
    s.ctl("dispatch", "focusmonitor", s.names[0])
    before = native(s)
    pinned_workspace = s.windows()["hs-C"]["workspace"]["id"]
    assert before[s.names[1]][1] != 0
    opened(s, wait_for)
    pickup(s, s.preview_point("hs-A"), 11)
    s.move(background(s, 1))
    release(s)
    finish(s, wait_for, 11, 1)
    assert native(s)[s.names[1]] == before[s.names[1]]
    assert s.windows()["hs-A"]["floating"] and s.windows()["hs-A"]["size"] == [300, 180]
    assert s.windows()["hs-C"]["pinned"] and s.windows()["hs-C"]["monitor"] == monitor(s, 0)["id"]
    assert s.windows()["hs-C"]["workspace"]["id"] == (native(s)[s.names[0]][0] if pinned_workspace == 11 else pinned_workspace)
    s.check("floating windows keep their size, native pinned windows remain on the source and destination regular/special activation is unchanged")

    for fullscreen in ("0", "1"):
        reset(s, wait_for, 2, 1)
        s.ctl("dispatch", "fullscreen", fullscreen)
        before = s.windows()["hs-A"]["fullscreen"]
        opened(s, wait_for)
        pickup(s, s.preview_point("hs-A"), 13)
        s.move(background(s, 1))
        release(s)
        finish(s, wait_for, 13, 1)
        assert s.windows()["hs-A"]["fullscreen"] == before
    s.check("fullscreen and maximized workspace members retain their native modes after a monitor transfer")

    reset(s, wait_for)
    s.ctl("keyword", "workspace", f"21,monitor:{s.names[0]},persistent:true")
    s.ctl("dispatch", "movetoworkspacesilent", f'11,address:{s.windows()["hs-C"]["address"]}')
    s.ctl("dispatch", "focuswindow", "address:" + s.windows()["hs-A"]["address"])
    s.ctl("dispatch", "togglegroup")
    s.ctl("dispatch", "focuswindow", "address:" + s.windows()["hs-C"]["address"])
    first, second = s.point(s.windows()["hs-A"]), s.point(s.windows()["hs-C"])
    dx, dy = first[0] - second[0], first[1] - second[1]
    direction = (("l" if dx < 0 else "r") if abs(dx) > abs(dy) else ("u" if dy < 0 else "d"))
    s.ctl("dispatch", "moveintogroup", direction)
    s.ctl("dispatch", "focuswindow", "address:" + s.windows()["hs-A"]["address"])
    grouped = set(s.windows()["hs-A"]["grouped"])
    assert len(grouped) == 2
    opened(s, wait_for)
    pickup(s, s.preview_point("hs-A"), 11)
    s.move(background(s, 1))
    # Native application placement follows the focused output. Map this late
    # member on the source before returning to the destination for the drop.
    s.move(background(s, 0))
    frozen = dict(s.status()["drag"]["box"])
    s.spawn(["python3", str(s.client), "hs-workspace-new"])
    wait_for(lambda: "hs-workspace-new" in s.windows())
    assert s.windows()["hs-workspace-new"]["workspace"]["id"] == 11
    assert s.status()["drag"]["box"] == frozen
    s.move(background(s, 1))
    release(s)
    finish(s, wait_for, 11, 1)
    assert native(s)[s.names[0]][0] == 21
    assert grouped <= set(s.windows()["hs-A"]["grouped"])
    for title in ("hs-A", "hs-C", "hs-workspace-new"):
        assert s.windows()[title]["workspace"]["id"] == 11
        assert s.windows()[title]["monitor"] == monitor(s, 1)["id"]
    s.check("source replacement uses an existing regular workspace, groups stay together and windows opened during dragging join the final move without changing the carried geometry")


def chords_and_thresholds(s, wait_for):
    reset(s, wait_for)
    assert json.loads(s.ctl("-j", "getoption", OPTION))["str"] == "SUPER ALT"
    opened(s, wait_for)
    for option, chord in (("SUPER ALT", (SUPER,)), ("SUPER ALT", (SUPER, ALT, SHIFT)), ("", (SUPER, ALT)), ("SUPER", (SUPER,)), ("ALT", (SUPER, ALT))):
        s.ctl("keyword", OPTION, option)
        s.move(center(tile(s, 13)))  # Empty tile avoids window-drag fallback.
        for code in chord:
            s.key(code, 1)
        s.button(1)
        assert not s.status()["dragging"], (option, chord, s.status())
        release(s, chord)
        assert s.status()["live"]
    s.ctl("keyword", OPTION, "SUPER CTRL")
    pickup(s, center(tile(s, 13)), 13, (SUPER, CTRL))
    s.move(background(s, 1))
    release(s, (SUPER, CTRL))
    finish(s, wait_for, 13, 1)
    s.check("workspace drag requires the exact configured native modifier chord, accepts Super+Control and can be disabled")

    for index in range(3):
        reset(s, wait_for, index, (index + 1) % 3)
        s.ctl("keyword", "binds:drag_threshold", "12")
        opened(s, wait_for)
        identity = 11 + index
        for distance, moved in ((0, False), (11, False), (12, False), (13, True)):
            pickup(s, s.preview_point("hs-A"), identity)
            start = s.data("cursorpos")
            s.move((start["x"] + distance, start["y"]))
            assert s.status()["drag"]["threshold"] == 12
            assert s.status()["drag"]["moved"] == moved, (index, distance, s.status()["drag"])
            s.run("wtype", "-k", "Escape")
            release(s)
            assert workspace(s, identity)["monitor"] == s.names[index]
        s.check(f"workspace threshold uses logical pointer displacement below, at and beyond the boundary on {s.names[index]}")

    reset(s, wait_for)
    opened(s, wait_for)
    for button, kind in ((272, "move"), (273, "resize")):
        s.move(s.preview_point("hs-A"))
        s.key(SUPER, 1)
        s.button(1, button)
        assert s.status()["dragging"] and s.status()["drag"]["kind"] == kind
        s.run("wtype", "-k", "Escape")
        s.button(0, button)
        s.key(SUPER, 0)
    s.ctl("dispatch", "movetoworkspacesilent", f'special:workspace-drag-source,address:{s.windows()["hs-A"]["address"]}')
    wait_for(lambda: s.windows()["hs-A"]["workspace"]["name"].startswith("special:"))
    special = s.windows()["hs-A"]["workspace"]["id"]
    wait_for(lambda: any(t["workspace"] == special for v in s.status()["views"] for t in v["tiles"]))
    s.move(s.preview_point("hs-A"))
    s.key(SUPER, 1)
    s.key(ALT, 1)
    s.button(1)
    assert not s.status()["dragging"]
    release(s)
    assert s.status()["live"]
    s.check("existing Super window move/resize controls retain their gestures; workspace pickup excludes scratchpads")


def invalid_and_cancelled(s, wait_for):
    reset(s, wait_for)
    opened(s, wait_for)
    before = native(s)
    for point in (background(s, 0),):
        pickup(s, s.preview_point("hs-A"), 11)
        s.move(point)
        assert s.status()["drag"]["target_monitor"] == ""
        release(s)
        assert workspace(s, 11)["monitor"] == s.names[0] and native(s) == before
    s.check("same-output releases leave workspace placement and native activation unchanged")

    s.close()
    s.ctl("keyword", "plugin:hyprspace:overview:all_monitors", "false")
    s.move(s.point(s.windows()["hs-A"]))
    opened(s, wait_for, 1)
    pickup(s, s.preview_point("hs-A"), 11)
    s.move(background(s, 1))
    assert s.status()["drag"]["target_monitor"] == ""
    release(s)
    assert workspace(s, 11)["monitor"] == s.names[0]
    s.check("outputs without an overview reject workspace drops")

    reset(s, wait_for, 1, 2)
    entry = s.root / "workspace-drag-panel"
    click = entry.with_name(entry.name + ".click")
    click.unlink(missing_ok=True)
    panel = s.spawn(["python3", str(s.artifact("layer.py")), str(entry), "waybar", "bottom"])
    try:
        box = wait_for(lambda: s.layer("waybar"))
        opened(s, wait_for)
        pickup(s, s.preview_point("hs-A"), 12)
        s.move((box["x"] + 70, box["y"] + 50))
        assert s.status()["dragging"] and s.status()["cursor_owned"]
        assert s.status()["drag"]["target_monitor"] == ""
        geometry = s.geometry()
        s.scroll()
        assert s.geometry() == geometry
        release(s)
        assert workspace(s, 12)["monitor"] == s.names[1] and not click.exists()
        s.check("workspace gestures capture panels and scroll but reject releases in reserved output space")
    finally:
        panel.terminate()
        panel.wait(timeout=3)

    reset(s, wait_for, 1, 2)
    entry = s.root / "workspace-drag-overlay"
    click = entry.with_name(entry.name + ".click")
    click.unlink(missing_ok=True)
    panel = s.spawn(["python3", str(s.artifact("layer.py")), str(entry), "hs-workspace-overlay", "pointer"])
    try:
        box = wait_for(lambda: s.layer("hs-workspace-overlay"))
        opened(s, wait_for)
        pickup(s, s.preview_point("hs-A"), 12)
        work = area(monitor(s, 0))
        point = (box["x"] + 70, box["y"] + 70)
        assert work["x"] < point[0] < work["x"] + work["w"] and work["y"] < point[1] < work["y"] + work["h"]
        s.move(point)
        assert s.status()["dragging"] and s.status()["cursor_owned"]
        assert s.status()["drag"]["target_monitor"] == ""
        release(s)
        assert workspace(s, 12)["monitor"] == s.names[1] and not click.exists()
        s.check("a pointer-only overlay inside usable output space rejects workspace drops without taking the gesture's input")
    finally:
        panel.terminate()
        panel.wait(timeout=3)

    for action in ("Escape", "close", "reload", "external move", "output removal"):
        reset(s, wait_for)
        opened(s, wait_for)
        pickup(s, s.preview_point("hs-A"), 11)
        s.move(background(s, 1))
        if action == "Escape":
            s.run("wtype", "-k", "Escape")
        elif action == "close":
            s.close()
        elif action == "reload":
            s.ctl("reload")
        elif action == "external move":
            s.ctl("dispatch", "moveworkspacetomonitor", f"11 {s.names[2]}")
        else:
            s.ctl("keyword", "monitor", f"{s.names[1]},disable")
            s.await_outputs(2)
        try:
            wait_for(lambda: not s.status()["dragging"])
            release(s)
            assert workspace(s, 11)["monitor"] == s.names[2 if action == "external move" else 0]
            assert s.status()["layout_targets_unique"]
            assert not s.status()["zoom"]["held"]
            if action != "close":
                assert s.status()["live"]
            s.check(f"{action} cancels a workspace gesture and its late release cannot commit")
        finally:
            if action == "output removal":
                s.ctl("keyword", "monitor", f"{s.names[1]},960x600@60,-1000x-200,1.25,transform,1")
                s.await_outputs(3)


def zoom_and_shortcuts(s, wait_for):
    for follow in (False, True):
        reset(s, wait_for)
        s.ctl("keyword", "plugin:hyprspace:follow_mouse", str(follow).lower())
        opened(s, wait_for)
        s.move(s.preview_point("hs-A"))
        s.key(102, 1)  # Home selects the source independently of follow_mouse.
        s.key(102, 0)
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        s.key(13, 1)
        s.key(13, 0)
        inspection(s, wait_for, 1.15)
        pickup(s, s.preview_point("hs-A"), 11)
        frozen = tile(s, 11)
        s.move(background(s, 1))
        s.scroll(-30, -2)
        assert all(abs(tile(s, 11)[key] - frozen[key]) < .1 for key in ("x", "y", "w", "h"))
        assert s.status()["target"]["workspace"] == 11
        release(s)
        finish(s, wait_for, 11, 1)
        wait_for(lambda: not s.status()["zoom"]["held"] and not s.status()["zoom"]["returning"])
        s.key(Z, 0)
        assert s.status()["target"]["workspace"] == 11
        s.check(f"follow_mouse={follow}: pickup from magnified previews freezes cameras and scroll, follows pointer destinations and selects the moved tile in the grid")

    reset(s, wait_for)
    s.ctl("keyword", "bind", "SUPER ALT,F,togglefloating")
    opened(s, wait_for)
    pickup(s, s.preview_point("hs-A"), 11)
    s.move(background(s, 1))
    s.key(33, 1)  # F dispatches normally with the initiating modifiers held.
    s.key(33, 0)
    wait_for(lambda: not s.status()["dragging"])
    release(s)
    assert s.windows()["hs-A"]["floating"] and not s.windows()["hs-B"]["floating"]
    assert workspace(s, 11)["monitor"] == s.names[0] and s.status()["live"]
    s.check("a native keyboard command cancels workspace dragging and still acts on the source selection")


def foreground_handoff(s, wait_for):
    reset(s, wait_for)
    opened(s, wait_for)
    pickup(s, s.preview_point("hs-A"), 11)
    s.move(background(s, 1))
    entry = s.root / "workspace-drag-foreground"
    click = entry.with_name(entry.name + ".click")
    click.unlink(missing_ok=True)
    foreground = s.spawn(["python3", str(s.artifact("layer.py")), str(entry)])
    try:
        box = wait_for(lambda: s.layer("hs-foreground"))
        wait_for(lambda: not s.status()["keyboard_owned"] and not s.status()["dragging"])
        assert workspace(s, 11)["monitor"] == s.names[0] and s.status()["live"]
        s.move((box["x"] + 70, box["y"] + 70))
        release(s)
        assert not click.exists()
        s.run("wtype", "drag")
        wait_for(lambda: entry.exists() and entry.read_text() == "drag")
        s.button(1)
        s.button(0)
        wait_for(click.exists)
        s.run("wtype", "-k", "Escape")
        foreground.wait(timeout=3)
        wait_for(lambda: s.status()["keyboard_owned"])
        assert s.status()["live"] and not s.status()["dragging"]
        s.check("exclusive foreground UI cancels workspace dragging, receives fresh typing/clicks and restores the still-open overview without a late drop")
    finally:
        if foreground.poll() is None:
            foreground.terminate()
            foreground.wait(timeout=3)


def input_lifecycle(s, wait_for):
    previous = s.env.get("WAYLAND_DEBUG")
    s.env["WAYLAND_DEBUG"] = "client"
    try:
        reset(s, wait_for)
    finally:
        if previous is None:
            s.env.pop("WAYLAND_DEBUG", None)
        else:
            s.env["WAYLAND_DEBUG"] = previous
    opened(s, wait_for)
    pickup(s, s.preview_point("hs-A"), 11)
    s.close()
    s.key(ALT, 0)
    s.key(SUPER, 0)
    s.move(s.point(s.windows()["hs-A"]))
    saved = offsets(s)
    s.button(0)
    s.motion(1, 0)
    wait_for(lambda: events(saved, r"wl_pointer#\d+\.motion\("))
    assert not events(saved, r"wl_pointer#\d+\.button\(")
    s.button(1)
    s.button(0)
    s.motion(1, 0)
    wait_for(lambda: len(events(saved, r"wl_pointer#\d+\.button\(")) == 2)
    s.check("closing a workspace gesture consumes its late release and the next native click receives a complete press/release pair")

    secondary = s.spawn([str(s.artifact("test-pointer"))], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    assert reply(secondary) == "ready"
    try:
        opened(s, wait_for)
        pickup(s, s.preview_point("hs-A"), 11, pointer=secondary)
        s.move(background(s, 1))
        secondary.terminate()
        secondary.wait(timeout=3)
        wait_for(lambda: not s.status()["dragging"])
        s.key(ALT, 0)
        s.key(SUPER, 0)
        assert workspace(s, 11)["monitor"] == s.names[0] and s.status()["live"]
        # No old button release is synthesized: the next real press must retire
        # the disconnected device's lease and allow a new overview gesture.
        pickup(s, s.preview_point("hs-A"), 11)
        s.move(background(s, 1))
        release(s)
        finish(s, wait_for, 11, 1)
        s.check("pointer disconnect cancels the old workspace drag and a fresh press can start another without an orphaned release")

        secondary = s.spawn([str(s.artifact("test-pointer"))], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        assert reply(secondary) == "ready"
        pickup(s, s.preview_point("hs-B"), 12, pointer=secondary)
        secondary.terminate()
        secondary.wait(timeout=3)
        wait_for(lambda: not s.status()["dragging"])
        s.close()
        s.key(ALT, 0)
        s.key(SUPER, 0)
        s.ctl("dispatch", "focuswindow", "address:" + s.windows()["hs-B"]["address"])
        s.move(s.point(s.windows()["hs-B"]))
        saved = offsets(s)
        s.button(1)
        s.button(0)
        s.motion(1, 0)
        wait_for(lambda: len(events(saved, r"wl_pointer#\d+\.button\(")) == 2)
        s.check("a fresh native click after pointer disconnect and overview closure receives a complete press/release pair without an old release")
    finally:
        if secondary.poll() is None:
            secondary.terminate()
            secondary.wait(timeout=3)

    reset(s, wait_for)
    opened(s, wait_for)
    pickup(s, s.preview_point("hs-A"), 11)
    locker = s.spawn([str(s.artifact("test-lock"))], stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)

    def request(command):
        locker.stdin.write(command.encode() + b"\n")
        locker.stdin.flush()
        return json.loads(reply(locker))

    try:
        assert reply(locker) == "ready"
        request("lock")
        wait_for(lambda: request("counts")["locked"])
        wait_for(lambda: not s.status()["views"])
        assert not s.status()["dragging"] and not s.status()["keyboard_owned"] and not s.status()["cursor_owned"]
        before = request("counts")["buttons"]
        release(s)
        assert request("counts")["buttons"] == before
        s.button(1)
        s.button(0)
        wait_for(lambda: request("counts")["buttons"] == before + 2)
        assert workspace(s, 11)["monitor"] == s.names[0]
        request("unlock")
        s.check("locking during workspace dragging restores input ownership, consumes the late release and routes a fresh click to the locker")
    finally:
        if locker.poll() is None:
            if request("counts")["locked"]:
                request("unlock")
            locker.terminate()
            locker.wait(timeout=3)


def settle_animation(s, wait_for):
    samples = {}

    def begin(source=0, destination=1, animate=True):
        reset(s, wait_for, source, destination)
        opened(s, wait_for)
        # Opening under the fixture's disabled animations keeps the measured
        # workspace grid stationary; only the drop transition is animated.
        s.ctl("keyword", "animations:enabled", str(animate).lower())
        identity = 11 + source
        before = native(s)
        pickup(s, s.preview_point("hs-A"), identity)
        s.move(background(s, destination))
        carried = dict(s.status()["drag"]["box"])
        s.button(0)
        state = s.status()
        assert state["live"] and not state["dragging"], state
        assert workspace(s, identity)["monitor"] == s.names[destination]
        assert native(s)[s.names[destination]] == before[s.names[destination]]
        assert native(s)[s.names[source]][2] == before[s.names[source]][2]
        return identity, carried, state

    def distance(first, second):
        return sum((first[part] - second[part]) ** 2 for part in ("x", "y", "w", "h")) ** .5

    for source, destination in ((0, 1), (1, 0)):
        identity, carried, state = begin(source, destination)
        frames = []
        deadline = time.monotonic() + 1
        while state.get("workspace_settle") and time.monotonic() < deadline:
            settle = state["workspace_settle"]
            assert settle["workspace"] == identity and settle["monitor"] == s.names[destination], settle
            assert not state["dragging"] and state["keyboard_owned"], state
            assert 0 <= settle["progress"] <= 1, settle
            goal, box = settle["destination"], settle["box"]
            for part in ("x", "y", "w", "h"):
                assert min(carried[part], goal[part]) - .25 <= box[part] <= max(carried[part], goal[part]) + .25, (carried, settle)
            assert settle["windows"], settle
            for slot in settle["windows"]:
                clip = slot["clip"]
                assert 0 <= slot["visibility"] <= 1, slot
                assert clip["x"] >= box["x"] - .25 and clip["y"] >= box["y"] - .25, settle
                assert clip["x"] + clip["w"] <= box["x"] + box["w"] + .25 and clip["y"] + clip["h"] <= box["y"] + box["h"] + .25, settle
            if frames:
                assert settle["progress"] >= frames[-1]["progress"], frames
                assert distance(box, goal) <= distance(frames[-1]["box"], goal) + .25, frames
            frames.append(settle)
            time.sleep(.01)
            state = s.status()
        assert not state.get("workspace_settle"), state
        assert len(frames) >= 2 and any(0 < frame["progress"] < 1 for frame in frames), frames
        assert distance(frames[-1]["box"], frames[-1]["destination"]) < distance(carried, frames[0]["destination"]), frames
        assert abs(carried["w"] - frames[0]["destination"]["w"]) > 1 and abs(carried["h"] - frames[0]["destination"]["h"]) > 1, frames
        final = tile(s, identity)
        assert all(abs(final[part] - frames[-1]["destination"][part]) < .25 for part in ("x", "y", "w", "h")), (final, frames)
        release(s)
        finish(s, wait_for, identity, destination)
        final_window = dict(s.preview("hs-A"))
        time.sleep(.2)
        assert not s.status().get("workspace_settle"), s.status()
        rested_window = s.preview("hs-A")
        assert all(abs(final_window[part] - rested_window[part]) < .25 for part in ("x", "y", "w", "h")), (final_window, rested_window)
        samples[f"{source}-to-{destination}"] = frames
        s.check(f"workspace drop glides and resizes into its final tile from {s.names[source]} to {s.names[destination]}, with native placement immediate and no lingering gesture or card")

    (s.root / "workspace-settle-frames.json").write_text(json.dumps(samples, indent=2))

    for disabled in ("global", "windowsMove"):
        reset(s, wait_for)
        opened(s, wait_for)
        s.ctl("keyword", "animations:enabled", str(disabled != "global").lower())
        if disabled == "windowsMove":
            s.ctl("keyword", "animation", "windowsMove,0,1.8,default")
        pickup(s, s.preview_point("hs-A"), 11)
        s.move(background(s, 1))
        s.button(0)
        assert not s.status().get("workspace_settle"), s.status()
        release(s)
        finish(s, wait_for, 11, 1)
        s.check(f"workspace drops complete immediately when {disabled} animations are disabled")

    for action in ("invalid drop", "Escape"):
        reset(s, wait_for)
        opened(s, wait_for)
        s.ctl("keyword", "animations:enabled", "true")
        pickup(s, s.preview_point("hs-A"), 11)
        s.move(background(s, 0 if action == "invalid drop" else 1))
        if action == "Escape":
            s.key(1, 1)
            s.key(1, 0)
        release(s)
        state = s.status()
        assert not state["dragging"] and not state.get("workspace_settle"), state
        assert workspace(s, 11)["monitor"] == s.names[0]
        s.check(f"{action} leaves native placement unchanged and creates no settle animation")

    for action in ("new pickup", "keyboard navigation", "native command", "close", "reload", "external move", "output removal"):
        identity, _, state = begin()
        assert state.get("workspace_settle"), state
        try:
            if action == "new pickup":
                s.move(center(tile(s, identity)))
                assert s.status().get("workspace_settle"), "settle finished before testing pickup takeover"
                s.button(1)
                state = s.status()
                assert state["dragging"] and state["drag"]["kind"] == "workspace", state
                assert state["drag"]["workspace"] == identity and not state.get("workspace_settle"), state
                s.key(1, 1)
                s.key(1, 0)
            elif action == "keyboard navigation":
                release(s)
                assert s.status().get("workspace_settle"), "settle finished before testing keyboard takeover"
                s.key(15, 1)  # Tab selects the next tile on the destination.
                s.key(15, 0)
                assert s.status()["target"]["workspace"] == 12, s.status()
            elif action == "native command":
                s.ctl("dispatch", "setfloating", "address:" + s.windows()["hs-A"]["address"])
                assert s.windows()["hs-A"]["floating"]
            elif action == "close":
                s.close()
                assert not s.status()["live"]
            elif action == "reload":
                s.ctl("reload")
            elif action == "external move":
                s.ctl("dispatch", "moveworkspacetomonitor", f"{identity} {s.names[2]}")
                assert workspace(s, identity)["monitor"] == s.names[2]
            else:
                s.ctl("keyword", "monitor", f"{s.names[1]},disable")
                s.await_outputs(2)
            assert not s.status().get("workspace_settle"), s.status()
            assert s.status()["layout_targets_unique"]
            if action not in ("external move", "output removal"):
                assert workspace(s, identity)["monitor"] == s.names[1]
            s.check(f"{action} cancels the cosmetic workspace settle without blocking input or undoing the completed native transfer")
        finally:
            release(s)
            if action == "output removal":
                s.ctl("keyword", "monitor", f"{s.names[1]},960x600@60,-1000x-200,1.25,transform,1")
                s.await_outputs(3)


def run(s, wait_for):
    try:
        transfers(s, wait_for)
        identities_and_windows(s, wait_for)
        chords_and_thresholds(s, wait_for)
        invalid_and_cancelled(s, wait_for)
        zoom_and_shortcuts(s, wait_for)
        foreground_handoff(s, wait_for)
        input_lifecycle(s, wait_for)
        settle_animation(s, wait_for)
    except BaseException:
        (s.root / "workspace-drag-failure.json").write_text(json.dumps({
            "status": s.status(), "workspaces": s.data("workspaces"),
            "monitors": s.data("monitors"), "clients": s.data("clients"),
        }, indent=2))
        raise
    finally:
        s.button(0)
        s.button(0, 273)
        for code in (SUPER, ALT, CTRL, SHIFT, Z, 13, 33):
            s.key(code, 0)
        s.close()
        s.ctl("reload")
