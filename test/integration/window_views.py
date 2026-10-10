"""Global window boards in the private compositor, including native targeting."""

import json
import subprocess
import time

from zoom import center, reply

G, SHIFT, Z, SPACE, PLUS = 34, 42, 44, 57, 13


def board(s):
    return s.status()["window_board"]


def tap(s, code):
    s.key(code, 1)
    s.key(code, 0)


def opened(s, wait_for, mode="flat"):
    s.ctl("dispatch", "hyprspace:overview", "on")
    wait_for(lambda: s.status()["live"])
    s.ctl("dispatch", "hyprspace:windowview", mode)
    return wait_for(lambda: board(s)["active"] and board(s))


def placement(s):
    return {title: (w["workspace"]["id"], w["monitor"], w["at"], w["size"], w["floating"], w["fullscreen"])
            for title, w in s.windows().items()}


def search(s, text):
    s.run("wtype", "-k", "slash")
    s.run("wtype", text)


def run(s, wait_for):
    s.setup("dwindle", 0, 1)
    original = placement(s)
    addresses = {w["address"] for w in s.windows().values()}
    original_focus = s.data("activewindow")["address"]
    s.ctl("dispatch", "hyprspace:overview", "on")
    wait_for(lambda: len(s.status()["views"]) == 3)
    s.move(s.preview_point("hs-A"))
    s.key(G, 1)
    wait_for(lambda: board(s)["active"])
    s.key(G, 1)  # A repeated press must not toggle a second time.
    assert board(s)["active"]
    s.key(G, 0)
    assert {p["window"] for p in board(s)["previews"]} == addresses
    assert board(s)["monitor"] == s.names[0]
    assert s.data("activewindow")["address"] == original_focus
    selected = board(s)["selected_window"]
    for mode in ("app", "workspace", "monitor", "flat"):
        s.ctl("dispatch", "hyprspace:windowview", mode)
        assert board(s)["grouping"] == mode and board(s)["count"] == 3
        assert board(s)["selected_window"] == selected
        assert {p["window"] for p in board(s)["previews"]} == addresses
        assert placement(s) == original
        assert s.data("activewindow")["address"] == original_focus
    s.key(SHIFT, 1)
    tap(s, G)
    s.key(SHIFT, 0)
    assert board(s)["grouping"] == "app"
    tap(s, 19)  # R
    assert board(s)["sort"] == "recent"
    order = [p["window"] for p in board(s)["previews"]]
    for _ in range(4):
        tap(s, 15)
    assert [p["window"] for p in board(s)["previews"]] == order
    s.check("global board includes every covered window; grouping preserves native placement and recency stays frozen")

    log = s.root / "input.log"
    log.write_text("")
    search(s, "HS-b")
    wait_for(lambda: board(s)["count"] == 1)
    assert board(s)["selected_window"] == s.windows()["hs-B"]["address"]
    s.run("wtype", " g rs 2")
    assert board(s)["active"] and board(s)["search_focused"]
    assert board(s)["query"] == "HS-b g rs 2", board(s)
    assert board(s)["count"] == 0
    s.run("wtype", "-k", "Return")
    assert s.status()["live"] and board(s)["active"]
    assert not log.read_text(), log.read_text()
    s.run("wtype", "-k", "Escape")
    assert board(s)["active"] and board(s)["count"] == 3 and not board(s)["search_focused"]
    s.run("wtype", "-k", "Escape")
    assert s.status()["live"] and not board(s)["active"]
    s.run("wtype", "-k", "Escape")
    wait_for(lambda: not s.status()["views"])
    assert placement(s) == original
    s.run("wtype", "g")
    wait_for(lambda: " 103" in log.read_text())
    s.check("search owns ordinary letters, spaces and digits; empty results cannot commit; Escape unwinds search and views")

    # A separate program identity supplies a second app group, with Unicode titles.
    title_file = s.root / "window-title"
    s.spawn(["env", "HS_APP_ID=hyprspace-research", f"HS_TITLE_FILE={title_file}",
             "python3", str(s.client), "hs-Æble Øvelse Århus"])
    wait_for(lambda: len(s.windows()) == 4)
    opened(s, wait_for, "app")
    wait_for(lambda: len(board(s)["groups"]) == 2)
    search(s, "æBLE øVELSE århus")
    wait_for(lambda: board(s)["count"] == 1)
    assert board(s)["selected_window"] == s.windows()["hs-Æble Øvelse Århus"]["address"]
    s.run("wtype", "-k", "BackSpace")
    assert board(s)["query"].endswith("århu")
    s.run("wtype", "-M", "ctrl", "u", "-m", "ctrl")
    assert board(s)["query"] == "" and board(s)["search_focused"]
    s.run("wtype", "renamed")
    assert board(s)["count"] == 0
    title_file.write_text("hs-Renamed Æble")
    wait_for(lambda: board(s)["count"] == 1)
    s.run("wtype", "-k", "Escape")
    s.close()
    s.check("canonical app groups separate applications; Danish search/editing and live title changes work")

    # Select a remote window through search, then verify native actions and Enter.
    s.ctl("dispatch", "focuswindow", "address:" + s.windows()["hs-A"]["address"])
    s.move(s.point(s.windows()["hs-A"], .5, .5))
    opened(s, wait_for)
    host = board(s)["monitor"]
    search(s, "hs-B")
    wait_for(lambda: board(s)["count"] == 1)
    selected = s.windows()["hs-B"]
    assert board(s)["previews"][0]["source_monitor"] != host
    s.ctl("keyword", "bind", "CTRL,F6,togglefloating")
    try:
        s.run("wtype", "-M", "ctrl", "-k", "F6", "-m", "ctrl")
        wait_for(lambda: s.windows()["hs-B"]["floating"])
        assert not s.windows()["hs-A"]["floating"]
        assert board(s)["monitor"] == host and board(s)["active"]
        s.run("wtype", "-M", "ctrl", "-k", "F6", "-m", "ctrl")
        wait_for(lambda: not s.windows()["hs-B"]["floating"])
    finally:
        s.ctl("keyword", "unbind", "CTRL,F6")
    s.run("wtype", "-k", "Return")
    wait_for(lambda: not s.status()["views"])
    assert s.data("activewindow")["address"] == selected["address"]
    assert s.data("activeworkspace")["id"] == selected["workspace"]["id"]
    s.check("native modified shortcuts and keyboard commits act on the remote source window without moving the board")

    s.ctl("dispatch", "focuswindow", "address:" + s.windows()["hs-A"]["address"])
    s.move(s.point(s.windows()["hs-A"], .5, .5))
    opened(s, wait_for)
    search(s, "hs-C")
    wait_for(lambda: board(s)["count"] == 1)
    preview = board(s)["previews"][0]
    s.move(center(preview))
    s.button(1)
    s.button(0)
    wait_for(lambda: not s.status()["views"])
    assert s.data("activewindow")["address"] == s.windows()["hs-C"]["address"]
    s.check("a global card click focuses its real window on the other output")

    opened(s, wait_for)
    s.move(center(next(p for p in board(s)["previews"] if p["window"] == board(s)["selected_window"])))
    before = placement(s)
    s.key(Z, 1)
    wait_for(lambda: board(s)["inspecting"])
    tap(s, PLUS)
    wait_for(lambda: abs(board(s)["inspection_factor"] - 1.15) < .001)
    s.key(SPACE, 1)
    s.motion(-20, -20)
    assert s.status()["zoom"]["pan_held"]
    s.key(SPACE, 0)
    s.button(1, 273)
    s.motion(20, 10)
    s.button(0, 273)
    s.scroll(delta=15, discrete=1)
    wait_for(lambda: abs(board(s)["inspection_factor"] - 1) < .001)
    s.key(Z, 0)
    assert not board(s)["inspecting"] and placement(s) == before
    s.close()
    s.check("window inspection reuses bounded zoom, keyboard grip and mouse pan without changing native geometry")

    for output in range(3):
        s.ctl("dispatch", "focusmonitor", s.names[output])
        s.ctl("keyword", "plugin:hyprspace:follow_mouse", "false")
        opened(s, wait_for)
        assert board(s)["monitor"] == s.names[output]
        s.close()
    s.ctl("keyword", "plugin:hyprspace:follow_mouse", "true")
    s.check("one global board renders and commits on every fractional-scale output")

    s.setup("dwindle", 0, 1)
    opened(s, wait_for)
    search(s, "hs-B")
    wait_for(lambda: board(s)["count"] == 1)
    s.ctl("dispatch", "closewindow", "address:" + s.windows()["hs-B"]["address"])
    wait_for(lambda: board(s)["count"] == 0)
    assert s.status()["live"]
    s.run("wtype", "-k", "Escape")
    assert board(s)["count"] == 2
    s.close()
    s.check("destroying a selected window removes its card without stale selection or focus")

    s.setup("dwindle", 0, 1)
    s.ctl("keyword", "plugin:hyprspace:overview:window_view_key", "x")
    s.ctl("dispatch", "hyprspace:overview", "on")
    tap(s, G)
    assert not board(s)["active"]
    s.key(45, 1)
    assert board(s)["active"]
    s.close()
    s.ctl("dispatch", "hyprspace:overview", "on")
    s.key(45, 1)
    assert not board(s)["active"]
    s.key(45, 0)
    tap(s, 45)
    assert board(s)["active"]
    s.close()
    s.ctl("keyword", "plugin:hyprspace:overview:window_view_key", "g")
    s.check("remapped toggle keys act once and their held repeats remain captured across close/reopen")

    lifecycle(s, wait_for)
    workspace_controls(s, wait_for)

    for mode in ("flat", "app", "workspace", "monitor"):
        opened(s, wait_for, mode)
        s.close()
        state = s.status()
        assert state["resources"]["captures"]["bytes"] == 0
        assert state["resources"]["textures"]["bytes"] == 0
        assert not state["keyboard_owned"] and not state["cursor_owned"]
    s.check("every window view releases captures, text/icon textures and input ownership on dismissal")


def lifecycle(s, wait_for):
    s.setup("dwindle", 0, 1)
    opened(s, wait_for)
    search(s, "abcdefghijklmnop")
    settings = {name: json.loads(s.ctl("-j", "getoption", name))["int"]
                for name in ("input:repeat_delay", "input:repeat_rate")}
    try:
        s.ctl("keyword", "input:repeat_delay", "100")
        s.ctl("keyword", "input:repeat_rate", "20")
        s.key(14, 1)  # Backspace repeats with the real keyboard's settings.
        wait_for(lambda: len(board(s)["query"]) <= 12)
        s.key(14, 0)
        remaining = board(s)["query"]
        time.sleep(.15)
        assert board(s)["query"] == remaining
        s.key(14, 1)
        s.close()
        opened(s, wait_for)
        s.run("wtype", "-k", "slash", "new query")
        s.key(14, 1)  # A hold from the old board must not edit this query.
        time.sleep(.25)
        assert board(s)["query"] == "new query", board(s)
        s.key(14, 0)
    finally:
        s.key(14, 0)
        for name, value in settings.items():
            s.ctl("keyword", name, str(value))
    s.check("held Backspace repeats, release stops it, and old holds cannot edit a reopened board")

    entry = s.root / "board-foreground-entry"
    foreground = s.spawn(["python3", str(s.artifact("layer.py")), str(entry)])
    try:
        wait_for(lambda: s.layer("hs-foreground") and not s.status()["keyboard_owned"])
        query = board(s)["query"]
        s.run("wtype", "g r z / 123")
        wait_for(lambda: entry.exists() and entry.read_text() == "g r z / 123")
        assert board(s)["query"] == query and board(s)["active"]
        s.run("wtype", "-k", "Escape")
        foreground.wait(timeout=3)
        wait_for(lambda: s.status()["keyboard_owned"])
        assert board(s)["query"] == query
        s.run("wtype", "-k", "Escape")
        assert board(s)["active"] and not board(s)["search_focused"]
    finally:
        if foreground.poll() is None:
            foreground.terminate()
            foreground.wait(timeout=3)
        s.close()
    s.check("foreground layers own typing and Escape; closing them restores board search without leaking keys")

    # Removing the board's output must relocate its presentation and cancel a lens hold.
    s.ctl("dispatch", "focusmonitor", s.names[1])
    s.ctl("keyword", "plugin:hyprspace:follow_mouse", "false")
    opened(s, wait_for, "monitor")
    assert board(s)["monitor"] == s.names[1]
    s.key(Z, 1)
    try:
        s.ctl("keyword", "monitor", f"{s.names[1]},disable")
        s.await_outputs(2)
        wait_for(lambda: board(s)["active"] and board(s)["monitor"] != s.names[1])
        assert board(s)["count"] == len(s.windows())
        assert not s.status()["zoom"]["held"] and not board(s)["inspecting"]
        assert len({p["window"] for p in board(s)["previews"]}) == board(s)["count"]
    finally:
        s.key(Z, 0)
        s.close()
        s.ctl("keyword", "monitor", f"{s.names[1]},960x600@60,-1000x-200,1.25,transform,1")
        s.await_outputs(3)
        s.ctl("keyword", "plugin:hyprspace:follow_mouse", "true")
    s.check("losing the board output relocates cards to a covered output and cancels inspection safely")

    s.setup("dwindle", 0, 1)
    opened(s, wait_for)
    s.key(Z, 1)
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
        state = s.status()
        assert not state["window_board"]["active"] and not state["zoom"]["held"]
        assert not state["keyboard_owned"] and not state["cursor_owned"]
        assert state["resources"]["captures"]["bytes"] == state["resources"]["textures"]["bytes"] == 0
        s.key(Z, 0)
        request("unlock")
        wait_for(lambda: not request("counts")["locked"])
    finally:
        s.key(Z, 0)
        if locker.poll() is None:
            if request("counts")["locked"]:
                request("unlock")
            locker.terminate()
            locker.wait(timeout=3)
    s.check("session locking closes the board and releases its capture, texture and input resources")

    opened(s, wait_for)
    s.key(Z, 1)
    tap(s, PLUS)
    s.ctl("plugin", "unload", str(s.plugin))
    s.key(Z, 0)
    s.ctl("plugin", "load", str(s.plugin))
    s.ctl("reload")
    state = s.status()
    assert not state["live"] and not state["views"] and not state["zoom"]["held"]
    assert state["resources"]["captures"]["bytes"] == state["resources"]["textures"]["bytes"] == 0
    opened(s, wait_for)
    assert board(s)["grouping"] == "flat" and board(s)["query"] == "" and board(s)["sort"] == "location"
    s.close()
    s.check("unloading an inspected board restores input and resources; reload starts with clean preferences")

    # A conflicting grouping shortcut yields to both existing configurable keys.
    for name, code in (("zoom_key", Z), ("empty_workspace_key", 49)):
        key = "z" if name == "zoom_key" else "n"
        s.ctl("keyword", "plugin:hyprspace:overview:window_view_key", key)
        s.ctl("dispatch", "hyprspace:overview", "on")
        s.key(code, 1)
        assert not board(s)["active"]
        if name == "zoom_key":
            assert s.status()["zoom"]["held"]
        s.key(code, 0)
        s.close()
    s.ctl("keyword", "plugin:hyprspace:overview:window_view_key", "")
    s.ctl("dispatch", "hyprspace:overview", "on")
    tap(s, G)
    assert not board(s)["active"]
    s.ctl("dispatch", "hyprspace:windowview", "app")
    assert board(s)["active"]
    s.close()
    s.ctl("keyword", "plugin:hyprspace:overview:window_view_key", "g")
    s.check("zoom and empty-workspace keys retain precedence; disabling the shortcut keeps dispatchers available")


def workspace_controls(s, wait_for):
    s.setup("dwindle", 0, 1)
    before = placement(s)
    for button in (False, True):
        opened(s, wait_for)
        preview = next(p for p in board(s)["previews"] if p["window"] == board(s)["selected_window"])
        s.move(center(preview))
        if button:
            s.button(1, 274)
            s.button(0, 274)
        else:
            tap(s, 49)  # N
        assert s.status()["live"] and not board(s)["active"]
        assert s.status()["target"]["window"] == "0x0"
        assert placement(s) == before
        s.close()
    opened(s, wait_for)
    s.run("wtype", "2")
    wait_for(lambda: not s.status()["views"])
    assert s.data("activeworkspace")["id"] == 2
    s.check("N, middle-click and workspace digits return to the native workspace controls")

    s.setup("dwindle", 0, 1)
    s.ctl("keyword", "plugin:hyprspace:overview:all_monitors", "false")
    try:
        opened(s, wait_for)
        assert board(s)["count"] == 1
        assert board(s)["previews"][0]["source_monitor"] == s.names[0]
    finally:
        s.close()
        s.ctl("keyword", "plugin:hyprspace:overview:all_monitors", "true")
    s.check("the global board respects the overview's covered-output scope")

    s.ctl("dispatch", "focuswindow", "address:" + s.windows()["hs-A"]["address"])
    s.ctl("dispatch", "fullscreen", "0")
    s.ctl("dispatch", "movetoworkspacesilent", "special:board,address:" + s.windows()["hs-C"]["address"])
    before = placement(s)
    opened(s, wait_for)
    assert board(s)["count"] == 3
    for mode in ("app", "workspace", "monitor", "flat"):
        s.ctl("dispatch", "hyprspace:windowview", mode)
        assert placement(s) == before and board(s)["count"] == 3
    s.close()
    s.ctl("keyword", "plugin:hyprspace:overview:include_special", "false")
    try:
        opened(s, wait_for)
        assert board(s)["count"] == 2
    finally:
        s.close()
        s.ctl("keyword", "plugin:hyprspace:overview:include_special", "true")
        s.ctl("dispatch", "focuswindow", "address:" + s.windows()["hs-A"]["address"])
        s.ctl("dispatch", "fullscreen", "0")
    s.check("grouping preserves native fullscreen and follows scratchpad inclusion settings")

    s.setup("dwindle", 0, 1)
    s.spawn(["env", "HS_CONTENT_MARKER=hs-board-live", "python3", str(s.client), "hs-board-live"])
    wait_for(lambda: len(s.windows()) == 4)
    address = s.windows()["hs-board-live"]["address"]
    s.ctl("dispatch", "movetoworkspacesilent", "12,address:" + address)
    s.ctl("dispatch", "focuswindow", "address:" + s.windows()["hs-A"]["address"])
    opened(s, wait_for)
    search(s, "hs-board-live")
    wait_for(lambda: board(s)["count"] == 1)
    host = next(monitor for monitor in s.data("monitors") if monitor["name"] == board(s)["monitor"])
    assert board(s)["previews"][0]["source_monitor"] != host["name"]
    time.sleep(.1)
    preview = board(s)["previews"][0]
    image_path = s.root / "window-board-capture.png"
    s.run("grim", "-s", "1", "-o", host["name"], str(image_path))
    from PIL import Image
    with Image.open(image_path) as screenshot:
        point = center(preview)
        pixel = screenshot.convert("RGB").getpixel((round(point[0] - host["x"]), round(point[1] - host["y"])))
        assert max(abs(a - b) for a, b in zip(pixel, (36, 229, 87))) <= 4, pixel
    s.ctl("dispatch", "movetoworkspacesilent", "11,address:" + address)
    wait_for(lambda: board(s)["previews"][0]["source_monitor"] == host["name"])
    assert board(s)["selected_window"] == address
    s.close()
    s.check("remote cards render the actual source pixels and track native workspace/monitor changes without losing selection")
