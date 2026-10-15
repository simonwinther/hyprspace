"""Global window boards in the private compositor, including native targeting."""

import json
import subprocess
import time

from zoom import assert_follow_framing, center, follow_corners, follow_during_zoom, inspection, relative_to, reply, same_geometry

G, F, SHIFT, Z, SPACE, PLUS = 34, 33, 42, 44, 57, 13


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


def overview_search(s, wait_for):
    s.setup("dwindle", 0, 1)
    chrome = s.spawn(["env", "HS_APP_ID=google-chrome", "python3", str(s.client),
                      "hs-Project roadmap", "hs-Release notes"])
    try:
        wait_for(lambda: len(s.windows()) == 5)
        project = s.windows()["hs-Project roadmap"]["address"]
        s.ctl("dispatch", "movetoworkspacesilent", "12,address:" + project)
        s.ctl("dispatch", "focuswindow", "address:" + s.windows()["hs-A"]["address"])
        original = placement(s)
        original_focus = s.data("activewindow")["address"]
        log = s.root / "input.log"
        log.write_text("")
        s.ctl("dispatch", "hyprspace:overview", "on")
        wait_for(lambda: s.status()["live"])
        s.move(s.preview_point("hs-A"))
        s.key(53, 1)  # / opens global search directly from workspace tiles.
        wait_for(lambda: board(s)["active"] and board(s)["search_focused"])
        assert board(s)["grouping"] == "flat" and board(s)["count"] == 5
        s.key(53, 1)
        time.sleep(.7)
        assert board(s)["query"] == ""
        assert placement(s) == original and s.data("activewindow")["address"] == original_focus

        # An opening press stays consumed when the overview closes and reopens.
        s.close()
        s.ctl("dispatch", "hyprspace:overview", "on")
        wait_for(lambda: s.status()["live"])
        s.key(53, 1)
        assert not board(s)["active"]
        s.key(53, 0)
        tap(s, 53)
        assert board(s)["search_focused"] and board(s)["query"] == ""
        s.run("wtype", "CHROME")
        wait_for(lambda: board(s)["count"] == 2)
        s.run("wtype", " project")
        wait_for(lambda: board(s)["count"] == 1)
        assert board(s)["selected_window"] == project
        assert placement(s) == original and s.data("activewindow")["address"] == original_focus
        assert not log.read_text(), log.read_text()
        s.run("wtype", "-k", "Return")
        wait_for(lambda: not s.status()["views"])
        assert s.data("activewindow")["address"] == project
        assert s.data("activeworkspace")["id"] == 12 and placement(s) == original

        # A prior grouping does not hide results when entering direct search.
        opened(s, wait_for, "app")
        s.ctl("dispatch", "hyprspace:windowview", "off")
        search(s, "release notes")
        wait_for(lambda: board(s)["count"] == 1)
        assert board(s)["grouping"] == "flat"
        assert board(s)["selected_window"] == s.windows()["hs-Release notes"]["address"]
        s.run("wtype", "-k", "Escape")
        assert board(s)["active"] and not board(s)["search_focused"] and board(s)["query"] == ""
        assert board(s)["count"] == 5
        s.run("wtype", "-k", "Escape")
        assert s.status()["live"] and not board(s)["active"]
        s.run("wtype", "-k", "Escape")
        wait_for(lambda: not s.status()["views"])
        s.check("/ from workspaces searches app names and titles across outputs; Enter focuses the native window and Escape unwinds search and views")
        s.check("the search opener acts once through repeats and close/reopen, without typing a slash or leaking input")
    finally:
        s.key(53, 0)
        s.close()
        chrome.terminate()
        chrome.wait(timeout=3)
        wait_for(lambda: len(s.windows()) == 3)


def scrolling(s, wait_for):
    fixture = s.artifact("test-overview.so")
    s.ctl("plugin", "load", str(fixture))
    try:
        scrolling_cases(s, wait_for)
    finally:
        s.close()
        s.ctl("plugin", "unload", str(fixture))


def scrolling_cases(s, wait_for):
    s.setup("dwindle", 0, 1)
    title_file = s.root / "scrolling-title"
    titles = [f"hs-scroll-{i}" for i in range(21)]
    s.spawn(["env", "HS_APP_ID=hyprspace-scroll", f"HS_TITLE_FILE={title_file}",
             "python3", str(s.client), *titles])
    wait_for(lambda: len(s.windows()) == 24)
    s.ctl("keyword", "plugin:hyprspace:follow_mouse", "true")
    s.ctl("keyword", "input:touchpad:scroll_factor", "1")
    s.ctl("keyword", "input:scroll_factor", "1")
    for mode in ("flat", "app", "workspace", "monitor"):
        opened(s, wait_for, mode)
        state = board(s)
        host = next(m for m in s.data("monitors") if m["name"] == state["monitor"])
        viewport = state["viewport"]
        s.move((host["x"] + viewport["x"] + viewport["w"] / 2,
                host["y"] + viewport["y"] + viewport["h"] / 2))
        tap(s, 102)  # Home selects the first card and reveals it.
        time.sleep(.35)
        origin = board(s)["scroll"]
        selected = board(s)["selected_window"]
        s.scroll(90, 0, source=2)  # Smooth finger input.
        before = board(s)
        assert abs(before["scroll"] - origin - 90) < .01, (mode, origin, before["scroll"])
        assert before["selected_window"] == selected
        title = f"hs-scroll-renamed-{mode}"
        title_file.write_text(title)
        wait_for(lambda: title in s.windows())
        time.sleep(.2)
        after = board(s)
        assert abs(after["scroll"] - before["scroll"]) < .01, (mode, before["scroll"], after["scroll"])
        assert after["selected_window"] == selected
        assert after["previews"] == before["previews"], (mode, before, after)

        # A frame without physical movement must not select the card now
        # under the pointer. Hovering clipped cards must not reveal them.
        s.motion(0, 0)
        assert board(s)["selected_window"] == selected
        assert abs(board(s)["scroll"] - before["scroll"]) < .01
        clipped = next(p for p in board(s)["previews"]
                       if p["y"] < host["y"] + viewport["y"]
                       < p["y"] + p["h"])
        s.move((clipped["x"] + clipped["w"] / 2,
                host["y"] + viewport["y"] + 3))
        assert board(s)["selected_window"] == clipped["window"]
        assert abs(board(s)["scroll"] - before["scroll"]) < .01
        s.scroll(-30, 0, source=2)
        assert abs(board(s)["scroll"] - origin - 60) < .01
        s.scroll(0, 0, source=2)  # End the touchpad gesture.
        s.scroll()
        assert abs(board(s)["scroll"] - (origin + 60 + viewport["h"] / 4)) < .01
        s.scroll(-1e6, -100)
        assert board(s)["scroll"] == 0
        s.scroll(-15, -1)
        assert board(s)["scroll"] == 0
        tap(s, 107)  # End reveals the last card through keyboard navigation.
        assert board(s)["scroll"] > 0
        tap(s, 102)
        assert board(s)["selected_window"] == board(s)["previews"][0]["window"]
        assert board(s)["scroll"] <= 34
        if mode == "flat":
            s.scroll(90, 0, source=2)
            selected = board(s)["selected_window"]
            added = s.spawn(["env", "HS_APP_ID=hyprspace-scroll", "python3", str(s.client), "hs-scroll-new"])
            try:
                wait_for(lambda: board(s)["count"] == 25)
                state = board(s)
                assert abs(state["scroll"] - 90) < .01 and state["selected_window"] == selected, (state["scroll"], selected, state["selected_window"])
            finally:
                added.terminate()
                added.wait(timeout=3)
            wait_for(lambda: board(s)["count"] == 24)
            state = board(s)
            assert abs(state["scroll"] - 90) < .01 and state["selected_window"] == selected, (state["scroll"], selected, state["selected_window"])
        s.ctl("dispatch", "hyprspace-test:board-scroll-continuity")
        s.close()
    s.check("all board groups preserve scrolling through live title updates, stationary frames and clipped-card hover; wheel, touchpad and keyboard bounds work")
    s.check("window membership updates preserve manual scroll and scrolling translates in-flight grouping animations without snapping")


def danish_search(s, wait_for):
    # wtype installs a keymap for its requested symbols. Use physical keycodes
    # with the user's Danish layout to exercise Shift+7 rather than a US slash.
    s.ctl("dispatch", "hyprspace:overview", "on")
    wait_for(lambda: s.status()["live"])
    before = placement(s)
    log = s.root / "input.log"
    log.write_text("")
    keyboard = s.spawn(
        ["env", "HS_KEYBOARD_LAYOUT=dk", "HS_KEYBOARD_VARIANT=nodeadkeys",
         str(s.artifact("test-pointer"))],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
    )
    assert reply(keyboard) == "ready"

    def key(code, state):
        keyboard.stdin.write(f"key {code} {state}\n")
        keyboard.stdin.flush()
        assert reply(keyboard) == "ok"

    def press(code):
        key(code, 1)
        key(code, 0)

    try:
        key(SHIFT, 1)
        key(8, 1)  # Danish / is Shift+7.
        wait_for(lambda: board(s)["search_focused"])
        key(8, 1)  # A repeat of the opening press must not type 7 or /.
        time.sleep(.7)
        assert board(s)["query"] == "" and board(s)["count"] == 3
        assert placement(s) == before
        key(8, 0)
        key(SHIFT, 0)
        key(SHIFT, 1)
        press(8)  # Once focused, another / is literal search text.
        key(SHIFT, 0)
        for code in (39, 40, 26):  # Danish æ, ø, å.
            press(code)
        key(SHIFT, 1)
        press(G)
        key(SHIFT, 0)
        press(8)
        assert board(s)["query"] == "/æøåG7", board(s)
        assert board(s)["active"] and board(s)["count"] == 0
        assert not log.read_text(), log.read_text()
        press(1)  # Escape clears the search without leaving the board.
        assert not board(s)["search_focused"] and board(s)["query"] == ""
        assert board(s)["count"] == 3 and placement(s) == before

        # A modified slash must still reach Hyprland's native binding matcher.
        s.ctl("keyword", "bind", "CTRL SHIFT,7,togglefloating")
        try:
            selected = board(s)["selected_window"]

            def floating():
                return next(w["floating"] for w in s.windows().values() if w["address"] == selected)

            original_floating = floating()
            key(29, 1)  # Ctrl
            key(SHIFT, 1)
            press(8)
            key(SHIFT, 0)
            key(29, 0)
            wait_for(lambda: floating() != original_floating)
            assert not board(s)["search_focused"]
            key(SHIFT, 1)
            press(8)
            key(SHIFT, 0)
            assert board(s)["search_focused"] and board(s)["query"] == ""
            key(29, 1)
            key(SHIFT, 1)
            press(8)
            key(SHIFT, 0)
            key(29, 0)
            wait_for(lambda: floating() == original_floating)
            assert board(s)["search_focused"] and board(s)["query"] == ""
        finally:
            s.ctl("keyword", "unbind", "CTRL SHIFT,7")
        press(1)
        s.close()
        s.check("Danish Shift+7 opens search directly from workspaces once; / and Danish letters type literally; modified shortcuts retain native routing")
    finally:
        for code in (8, SHIFT, 29):
            if keyboard.poll() is None:
                key(code, 0)
        keyboard.terminate()
        keyboard.wait(timeout=3)


def search_field(s, wait_for):
    opened(s, wait_for)
    search(s, "hs-")
    assert board(s)["search_cursor"] == 3
    selected = board(s)["selected_window"]
    s.run("wtype", "-k", "Left", "X")
    assert board(s)["query"] == "hsX-" and board(s)["search_cursor"] == 3
    assert board(s)["selected_window"] == selected or board(s)["count"] == 0
    s.run("wtype", "-k", "BackSpace", "-k", "Delete")
    assert board(s)["query"] == "hs" and board(s)["search_cursor"] == 2
    s.run("wtype", "-k", "Home", "æ", "-M", "shift", "-k", "End", "-m", "shift")
    assert board(s)["search_selection"] == [2, 4]
    s.run("wtype", "ø")
    assert board(s)["query"] == "æø" and board(s)["search_cursor"] == 4
    s.run("wtype", "-M", "ctrl", "a", "-m", "ctrl", "hs-")
    assert board(s)["query"] == "hs-" and board(s)["count"] == 3
    before = board(s)
    s.run("wtype", "-k", "Tab")
    assert board(s)["selected_window"] != before["selected_window"]
    assert board(s)["query"] == before["query"] and board(s)["search_cursor"] == before["search_cursor"]
    s.run("wtype", "-M", "ctrl", "a", "-m", "ctrl", "æøå", "-k", "Home", "-k", "Right")
    caret = board(s)["search_caret"]
    host = next(m for m in s.data("monitors") if m["name"] == board(s)["monitor"])
    s.run("wtype", "-k", "End")
    s.move((host["x"] + caret["x"] + caret["w"] / 2, host["y"] + caret["y"] + caret["h"] / 2))
    assert s.status()["cursor_shape"] == "text"
    s.button(1)
    s.button(0)
    assert board(s)["search_cursor"] == 2
    s.run("wtype", "G")
    assert board(s)["query"] == "æGøå"
    assert board(s)["search_caret_visible"]
    wait_for(lambda: not board(s)["search_caret_visible"])
    wait_for(lambda: board(s)["search_caret_visible"])
    s.run("wtype", "-M", "ctrl", "a", "-m", "ctrl", "gh")
    s.run("grim", "-s", "1", "-o", host["name"], str(s.root / "search-field-short.png"))
    s.run("wtype", "-M", "ctrl", "a", "-m", "ctrl")
    s.run("grim", "-s", "1", "-o", host["name"], str(s.root / "search-field-selection.png"))
    s.close()
    s.check("search has a shaped blinking caret, insertion, Unicode selection/deletion, click placement and independent result navigation")

    for output in range(3):
        s.ctl("dispatch", "focusmonitor", s.names[output])
        s.ctl("keyword", "plugin:hyprspace:follow_mouse", "false")
        opened(s, wait_for)
        search(s, "W" * 256)
        state = board(s)
        field, caret = state["search_box"], state["search_caret"]
        assert state["search_offset"] > 0 and caret["w"] > 0
        assert field["x"] + 12 <= caret["x"] <= field["x"] + field["w"] - 13 + .001
        assert field["y"] + 6 <= caret["y"] and caret["y"] + caret["h"] <= field["y"] + field["h"] - 6 + .001
        s.run("wtype", "-k", "Home")
        assert board(s)["search_offset"] == 0
        s.run("wtype", "-k", "End")
        assert board(s)["search_offset"] > 0
        if output == 0:
            s.run("grim", "-s", "1", "-o", s.names[output], str(s.root / "search-field-long.png"))
        s.close()
    s.ctl("keyword", "plugin:hyprspace:follow_mouse", "true")
    s.check("long search queries scroll their text and keep the caret inside the field on all fractional-scale outputs")

    opened(s, wait_for)
    search(s, "pending blink")
    assert board(s)["search_caret_visible"]
    s.ctl("plugin", "unload", str(s.plugin))
    time.sleep(1.1)  # Let a cancelled blink deadline pass after the code unloads.
    assert len(s.data("monitors")) == 3
    assert not any(p["name"] == "hyprspace" for p in json.loads(s.ctl("-j", "plugin", "list")))
    s.ctl("plugin", "load", str(s.plugin))
    s.ctl("reload")
    assert not s.status()["views"] and not s.status()["keyboard_owned"]
    assert s.ctl("configerrors") == ""
    s.check("unloading a focused search cancels its blink callback and reload restores clean input ownership")


def cursor_follow(s, wait_for):
    panel = None
    def chosen():
        state = board(s)
        return next(p for p in state["previews"] if p["window"] == state["selected_window"])

    def settle():
        wait_for(lambda: not s.status()["zoom"]["cursor_follow_settling"])

    try:
        s.setup("dwindle", 0, 1)
        native = placement(s)
        opened(s, wait_for)
        host = board(s)["monitor"]
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        fitted = chosen()
        s.move(center(fitted))
        tap(s, F)
        assert s.status()["zoom"]["cursor_follow_enabled"]
        s.scroll(delta=-480, discrete=-32)
        inspection(s, wait_for, 4)
        before = chosen()
        s.ctl("keyword", "plugin:hyprspace:follow_mouse", "false")
        selected = board(s)["selected_window"]
        for _ in range(3):
            s.motion(1, 0)
        time.sleep(.12)
        assert not s.status()["zoom"]["cursor_follow_settling"]
        assert same_geometry({0: before}, {0: chosen()})
        relative_to(s, (fitted["x"] + fitted["w"] * .85, fitted["y"] + fitted["h"] * .75))
        wait_for(lambda: s.status()["zoom"]["cursor_follow_settling"])
        assert not s.status()["zoom"]["camera_settled"]
        settle()
        followed = chosen()
        pointer = s.data("cursorpos")
        assert abs(followed["x"] - (fitted["x"] * 4 - pointer["x"] * 3)) < .1
        assert abs(followed["y"] - (fitted["y"] * 4 - pointer["y"] * 3)) < .1
        assert followed["x"] < before["x"] and followed["y"] < before["y"]
        assert board(s)["selected_window"] == selected and placement(s) == native
        s.check("G-view inspection shares bounded follow, its calm zone and settled status without changing selection or native placement")

        follow_corners(s, wait_for, host, fitted, chosen, "cursor-follow-board")
        s.check("G-view follow also reaches all full-output corners and resumes inward immediately")
        monitor = next(m for m in s.data("monitors") if m["name"] == host)
        namespace = "waybar-cursor-follow-board"
        panel = s.spawn(["python3", str(s.artifact("layer.py")), str(s.root / namespace), namespace, "pointer", f'{monitor["x"]},{monitor["y"]}'])
        box = wait_for(lambda: (value if (value := s.layer(namespace)) and value["w"] > 0 and value["h"] > 0 else None))
        relative_to(s, center(box))
        wait_for(lambda: s.status()["zoom"]["cursor_follow_settling"])
        settle()
        assert s.status()["cursor_owned"] and s.status()["keyboard_owned"]
        assert_follow_framing(fitted, chosen(), s.data("cursorpos"))
        s.ctl("keyword", "plugin:hyprspace:overview:show_follow_hint", "false")
        assert not s.status()["zoom"]["cursor_follow_hint"] and s.status()["zoom"]["cursor_follow_enabled"]
        s.ctl("keyword", "plugin:hyprspace:overview:show_follow_hint", "true")
        panel.terminate()
        panel.wait(timeout=3)
        panel = None
        s.check("Waybar cannot interrupt G-view follow, and hiding its hint retains the saved mode")

        s.move(center(fitted))
        s.scroll(delta=480, discrete=32)
        inspection(s, wait_for, 1)
        s.scroll(delta=-480, discrete=-32)
        inspection(s, wait_for, 4)
        follow_during_zoom(s, wait_for, fitted, chosen)
        s.check("a single immediate mouse movement steers an unfinished G-view zoom without freezing magnification or requiring another motion")
        s.key(PLUS, 1)
        s.motion(20, 10)
        assert s.status()["zoom"]["cursor_follow_settling"] and s.status()["zoom"]["inspection_transitioning"]
        s.key(PLUS, 0)
        inspection(s, wait_for, 4 / 1.15)
        assert_follow_framing(fitted, chosen(), s.data("cursorpos"), 4 / 1.15)
        s.check("G-view follows during a held keyboard zoom key and reaches the requested scale")
        s.ctl("keyword", "animations:enabled", "false")
        s.scroll(delta=480, discrete=32)
        inspection(s, wait_for, 1)
        s.move(center(fitted))
        s.scroll(delta=-480, discrete=-32)
        inspection(s, wait_for, 4)

        tap(s, F)
        stopped = chosen()
        s.motion(-30, -20)
        time.sleep(.12)
        assert same_geometry({0: stopped}, {0: chosen()})
        tap(s, F)
        s.move(center(fitted))
        time.sleep(.12)
        assert same_geometry({0: stopped}, {0: chosen()})
        s.motion(10, 0)
        settle()
        s.button(1, 273)
        grip = chosen()
        s.motion(20, 10)
        panned = chosen()
        assert abs(panned["x"] - grip["x"] - 20) < .1
        assert abs(panned["y"] - grip["y"] - 10) < .1
        assert not s.status()["zoom"]["cursor_follow_settling"]
        s.button(0, 273)
        time.sleep(.12)
        assert same_geometry({0: panned}, {0: chosen()})
        s.key(SPACE, 1)
        s.motion(-20, -10)
        assert s.status()["zoom"]["panning"] and not s.status()["zoom"]["cursor_follow_settling"]
        s.key(SPACE, 0)
        s.scroll(delta=480, discrete=32)
        inspection(s, wait_for, 1)
        assert same_geometry({0: fitted}, {0: chosen()})
        s.key(Z, 0)
        tap(s, G)
        assert not board(s)["active"] and s.status()["zoom"]["cursor_follow_enabled"]
        tap(s, G)
        assert board(s)["active"] and s.status()["zoom"]["cursor_follow_enabled"]
        assert not s.status()["zoom"]["cursor_follow_settling"]
        s.check("G-view manual grabs override follow, wheel-out restores the exact fit and view changes retain only the toggle")

        # Inspect a window whose native output differs from the presentation host.
        monitors = {m["id"]: m["name"] for m in s.data("monitors")}
        remote = next(w for w in s.windows().values() if monitors[w["monitor"]] != host)
        for _ in range(board(s)["count"]):
            if board(s)["selected_window"] == remote["address"]:
                break
            tap(s, 15)
        assert board(s)["selected_window"] == remote["address"]
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        fitted = chosen()
        s.move(center(fitted))
        s.scroll(delta=-480, discrete=-32)
        inspection(s, wait_for, 4)
        relative_to(s, (fitted["x"] + fitted["w"] * .7, fitted["y"] + fitted["h"] * .65))
        wait_for(lambda: s.status()["zoom"]["cursor_follow_settling"])
        settle()
        assert board(s)["monitor"] == host and board(s)["selected_window"] == remote["address"]
        assert placement(s) == native
        # A point in the fitted footprint stays visible at every pan bound.
        s.move(center(fitted))
        s.button(1)
        s.button(0)
        s.key(Z, 0)
        wait_for(lambda: not s.status()["views"])
        assert s.data("activewindow")["address"] == remote["address"]
        s.check("remote-window follow advances on the board's display output and clicks commit its native source window")

        opened(s, wait_for)
        assert s.status()["zoom"]["cursor_follow_enabled"]
        assert not s.status()["zoom"]["cursor_follow_settling"]
        search(s, "f")
        assert board(s)["query"] == "f" and s.status()["zoom"]["cursor_follow_enabled"]
        s.check("a new board opening retains enabled follow while F remains ordinary text in search")
        s.run("wtype", "-k", "Escape")
        s.key(Z, 1)
        inspection(s, wait_for, 1)
        tap(s, F)
        assert not s.status()["zoom"]["cursor_follow_enabled"]
        s.key(Z, 0)
        s.close()
        opened(s, wait_for)
        assert not s.status()["zoom"]["cursor_follow_enabled"]
        assert not s.status()["zoom"]["cursor_follow_settling"]
        s.check("disabling follow in the board persists across later openings")
    finally:
        if panel is not None and panel.poll() is None:
            panel.terminate()
            panel.wait(timeout=3)
        for code in (Z, F, SPACE, PLUS):
            s.key(code, 0)
        s.button(0, 273)
        s.ctl("keyword", "animations:enabled", "false")
        s.ctl("keyword", "plugin:hyprspace:follow_mouse", "true")
        s.ctl("keyword", "plugin:hyprspace:overview:show_follow_hint", "true")
        s.close()


def run(s, wait_for):
    overview_search(s, wait_for)
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

    danish_search(s, wait_for)
    search_field(s, wait_for)

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

    cursor_follow(s, wait_for)

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

    scrolling(s, wait_for)
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
