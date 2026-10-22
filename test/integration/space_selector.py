"""Space navigation rail and dialogs in the suite's private compositor only."""

import json
import subprocess
import time

from spaces import all_outputs


def tap(s, code):
    s.key(code, 1)
    s.key(code, 0)


def save(s):
    s.key(29, 1)
    tap(s, 28)
    s.key(29, 0)


def dismiss(s):
    for _ in range(20):
        if not selector(s)["active"]:
            return
        tap(s, 1)
    raise AssertionError(("selector parent stack did not close", selector(s)))


def selector(s):
    return s.status()["space_selector"]


def model(s):
    return s.status()["spaces"]


def dispatch(s, request):
    return s.ctl("dispatch", "hyprspace:space", json.dumps(request, ensure_ascii=False))


def opened(s, wait_for):
    s.ctl("dispatch", "hyprspace:overview", "on")
    wait_for(lambda: s.status()["live"] and s.status()["keyboard_owned"] and len(s.status()["views"]) == len(s.names))


def button(s, action, identity=None, monitor=None):
    target_monitor = s.names[monitor] if monitor is not None else selector(s).get("monitor", s.names[0])
    choices = [value for value in selector(s)["buttons"] if value["action"] == action
               and value["monitor"] == target_monitor and (identity is None or value["id"] == identity)]
    if not choices and action == "create":
        if selector(s)["input_mode"] == "insert":
            tap(s, 1)
        tap(s, 57)
        tap(s, 30)
        assert selector(s)["mode"] == "create", selector(s)
        return
    if not choices and action == "pick":
        if not selector(s)["active"]:
            button(s, "sidebar", monitor=monitor)
        if selector(s)["input_mode"] == "insert":
            tap(s, 1)
        tap(s, 57)
        tap(s, 25)  # Space p opens the full manager.
        assert selector(s)["mode"] == "pick", selector(s)
        return
    if not choices and action == "switch" and not selector(s)["active"]:
        button(s, "sidebar", monitor=monitor)
        choices = [value for value in selector(s)["buttons"] if value["action"] == action
                   and value["monitor"] == target_monitor and (identity is None or value["id"] == identity)]
    if not choices and action in ("rename", "remove", "move"):
        button(s, "menu", monitor=monitor)
        choices = [value for value in selector(s)["buttons"] if value["action"] == action
                   and value["monitor"] == target_monitor]
    assert choices, (action, identity, selector(s))
    box = choices[0]["box"]
    s.move((box["x"] + box["w"] / 2, box["y"] + box["h"] / 2))
    s.button(1)
    s.button(0)


def select_space(s, wait_for, identity):
    if not selector(s)["sidebar_visible"]:
        button(s, "sidebar")
    if any(value["action"] == "switch" and value["id"] == identity for value in selector(s)["buttons"]):
        button(s, "switch", identity)
    else:
        button(s, "pick")
        button(s, "destination", identity)
        button(s, "confirm")
    wait_for(lambda: model(s)["active"] == identity and all_outputs(s, identity) and s.status()["live"] and not selector(s)["active"])


def owner(s, title):
    native = s.windows()[title]["workspace"]["id"]
    return next(space["id"] for space in model(s)["spaces"]
                if any(workspace["native_id"] == native for workspace in space["workspaces"]))


def capture(s, name, monitor=None):
    output = monitor or selector(s).get("monitor", s.names[0])
    scale = next(value["scale"] for value in s.data("monitors") if value["name"] == output)
    s.run("grim", "-s", str(scale), "-o", output, str(s.root / name))


def searchable_icons(s, wait_for):
    """Run with an open overview in the suite's private compositor."""
    baseline = model(s)["active"]
    original = {space["id"] for space in model(s)["spaces"]}
    try:
        button(s, "create")
        s.run("wtype", "Searchable icon draft")
        tap(s, 15)  # Tab from Name to Icon.
        assert selector(s)["field"] == "icon" and selector(s)["input_mode"] == "normal"
        s.run("wtype", "/school")
        wait_for(lambda: selector(s)["query"] == "school")
        assert selector(s)["icon_matches"] == 1 and selector(s)["icon"] == "graduation-cap"
        assert selector(s)["input"] == "Searchable icon draft"
        capture(s, "icons-search-school.png")
        tap(s, 28)
        assert selector(s)["field"] == "icon" and selector(s)["input_mode"] == "normal"
        s.run("wtype", "?")
        wait_for(lambda: s.status()["help"]["active"])
        tap(s, 1)
        wait_for(lambda: not s.status()["help"]["active"])
        assert selector(s)["query"] == "school" and selector(s)["icon"] == "graduation-cap"
        assert selector(s)["field"] == "icon" and selector(s)["input"] == "Searchable icon draft"
        tap(s, 23)  # i resumes the icon search, never Name.
        s.key(29, 1)
        tap(s, 22)
        s.key(29, 0)
        s.run("wtype", "no-such-icon-zzzz")
        wait_for(lambda: selector(s)["icon_matches"] == 0)
        assert selector(s)["icon"] == "graduation-cap"
        assert not any(row["action"] == "icon" for row in selector(s)["buttons"])
        capture(s, "icons-search-empty.png")
        tap(s, 1)
        assert selector(s)["input_mode"] == "normal" and selector(s)["mode"] == "create"
        tap(s, 23)
        s.key(29, 1)
        tap(s, 22)
        s.key(29, 0)
        tap(s, 1)
        assert selector(s)["query"] == "" and selector(s)["icon_matches"] == 48
        assert selector(s)["icon"] == "graduation-cap"
        tap(s, 102)  # Home first, then Page Down.
        tap(s, 109)
        viewport = selector(s)["icon_viewport"]
        assert viewport["first"] > 0 and viewport["total"] == 48
        icons = [row for row in selector(s)["buttons"] if row["action"] == "icon"]
        assert len(icons) <= viewport["columns"] * viewport["rows"] <= 24
        assert any(row["id"] == selector(s)["icon"] and row["focused"] for row in icons)
        tap(s, 107)  # End selects the last icon, scrolls into view.
        assert selector(s)["icon"] == "sparkles"
        assert any(row["id"] == "sparkles" for row in selector(s)["buttons"])
        capture(s, "icons-last-page.png")
        button(s, "icon_previous")
        assert selector(s)["icon"] != "sparkles"
        button(s, "icon_next")
        assert selector(s)["icon"] == "sparkles"
        s.run("wtype", "/")
        s.run("wtype", "photography")
        wait_for(lambda: selector(s)["icon"] == "camera" and selector(s)["icon_matches"] == 1)
        save(s)  # Saves from active icon text editing.
        identity = wait_for(lambda: next((space["id"] for space in model(s)["spaces"] if space["name"] == "Searchable icon draft"), None))
        wait_for(lambda: model(s)["active"] == identity and all_outputs(s, identity))
        appearance = next(space for space in model(s)["spaces"] if space["id"] == identity)
        assert appearance["icon"] == "camera"
        button(s, "rename")
        tap(s, 15)
        assert selector(s)["icon"] == "camera" and selector(s)["query"] == ""
        s.run("wtype", "/mobile")
        wait_for(lambda: selector(s)["icon"] == "smartphone")
        tap(s, 28)
        save(s)
        wait_for(lambda: next(space for space in model(s)["spaces"] if space["id"] == identity)["icon"] == "smartphone")
        dismiss(s)
        s.check("48 searchable icons support aliases, no-results, draft-safe help, keyboard paging, pointer paging and create/edit persistence")
    finally:
        dismiss(s)
        dispatch(s, {"action": "switch", "id": baseline})
        for space in list(model(s)["spaces"]):
            if space["id"] not in original:
                dispatch(s, {"action": "delete", "id": space["id"], "target": baseline})


def reorder_spaces(s, wait_for):
    """Reorder visible siblings in both choosers without changing spaces."""
    baseline = model(s)["active"]
    original = {space["id"] for space in model(s)["spaces"]}
    identities = {}
    def order():
        return [space["id"] for space in model(s)["spaces"]]
    def shift(code):
        s.key(42, 1)
        tap(s, code)
        s.key(42, 0)
    try:
        for name in ("Reorder Alpha", "Hidden sibling", "Reorder Beta", "Reorder Gamma"):
            dispatch(s, {"action": "create", "name": name})
            identities[name] = wait_for(lambda: next((space["id"] for space in model(s)["spaces"] if space["name"] == name), None))
        dispatch(s, {"action": "switch", "id": baseline})
        wait_for(lambda: model(s)["active"] == baseline and all_outputs(s, baseline))
        if not selector(s)["sidebar_visible"]:
            button(s, "sidebar")
        tap(s, 57)
        tap(s, 25)  # Space p manages spaces without a mouse action footer.
        assert selector(s)["mode"] == "pick"
        s.run("wtype", "/Reorder")
        tap(s, 28)
        beta, gamma = identities["Reorder Beta"], identities["Reorder Gamma"]
        alpha, hidden = identities["Reorder Alpha"], identities["Hidden sibling"]
        button(s, "destination", beta)
        shift(36)  # Shift+J moves Beta after visible Gamma.
        wait_for(lambda: order().index(beta) > order().index(gamma))
        assert order().index(alpha) < order().index(hidden) < order().index(gamma) < order().index(beta)
        assert selector(s)["destination"] == beta and selector(s)["query"] == "Reorder" and model(s)["active"] == baseline
        boundary = order()
        shift(36)
        assert order() == boundary
        shift(37)  # Shift+K restores Beta before Gamma.
        wait_for(lambda: order().index(beta) < order().index(gamma))
        shift(37)  # It passes the hidden sibling and goes before visible Alpha.
        wait_for(lambda: order().index(beta) < order().index(alpha))
        boundary = order()
        shift(37)
        assert order() == boundary and selector(s)["destination"] == beta
        capture(s, "spaces-reorder-manager.png")
        s.run("wtype", "/JK")
        assert selector(s)["query"] == "ReorderJK" and selector(s)["input_mode"] == "insert"
        assert order() == boundary
        s.key(29, 1)
        tap(s, 22)
        s.key(29, 0)
        s.run("wtype", "Reorder")
        tap(s, 1)
        button(s, "cancel")
        assert selector(s)["mode"] == "sidebar"
        s.run("wtype", "/Reorder")
        tap(s, 28)
        tap(s, 102)
        assert selector(s)["destination"] == beta
        shift(36)
        wait_for(lambda: order().index(beta) > order().index(alpha))
        assert selector(s)["destination"] == beta and model(s)["active"] == baseline and all_outputs(s, baseline)
        tap(s, 107)
        assert selector(s)["destination"] == gamma
        boundary = order()
        shift(36)
        assert order() == boundary
        capture(s, "spaces-reorder-sidebar.png")
        dismiss(s)
        s.check("Shift+J/K reorders filtered visible siblings in manager and sidebar, preserves selection/current space, stops at boundaries and stays literal while searching")
    finally:
        s.key(42, 0)
        dismiss(s)
        dispatch(s, {"action": "switch", "id": baseline})
        for space in list(model(s)["spaces"]):
            if space["id"] not in original:
                dispatch(s, {"action": "delete", "id": space["id"], "target": baseline})


def run(s, wait_for, check_drag=True):
    s.setup("dwindle", 0, 1)
    baseline = model(s)["active"]
    original = {space["id"] for space in model(s)["spaces"]}
    original_monitor = next(value for value in s.data("monitors") if value["name"] == s.names[0])
    resized = False
    try:
        s.ctl("keyword", "plugin:hyprspace:overview:font", "Sans 12")
        font = json.loads(s.ctl("-j", "getoption", "plugin:hyprspace:overview:font"))
        assert font["str"] == "Sans 12", font
        (s.root / "space-selector-font.json").write_text(json.dumps(font, indent=2))
        s.ctl("keyword", "plugin:hyprspace:follow_mouse", "true")
        attempt = subprocess.run(["hyprctl", "dispatch", "hyprspace:sidebar", "on"], env=s.env, capture_output=True, text=True, timeout=10)
        assert "requires an open overview" in attempt.stdout + attempt.stderr and not s.status()["live"]
        opened(s, wait_for)
        capture(s, "spaces-closed.png")
        geometry = selector(s)["layout"][s.names[0]]
        assert not selector(s)["sidebar_visible"] and geometry["rail"]["w"] == 0
        context = next(value for value in selector(s)["buttons"] if value["action"] == "sidebar" and value["monitor"] == s.names[0])
        assert context["label"] == next(space["name"] for space in model(s)["spaces"] if space["id"] == baseline)
        assert geometry["header"]["y"] + geometry["header"]["h"] < geometry["content"]["y"]
        assert not any(value["action"] in ("rename", "remove", "move") for value in selector(s)["buttons"])
        canvas = [{"monitor": view["monitor"], "tiles": view["tiles"]} for view in s.status()["views"]]
        s.key(29, 1)
        s.key(57, 1)
        wait_for(lambda: selector(s)["sidebar_visible"])
        time.sleep(.7)
        assert selector(s)["sidebar_visible"], "a held Ctrl+Space toggled repeatedly"
        s.key(57, 0)
        s.key(29, 0)
        assert selector(s)["layout"][s.names[0]]["content"] == geometry["content"]
        assert [{"monitor": view["monitor"], "tiles": view["tiles"]} for view in s.status()["views"]] == canvas
        assert all(value["action"] == "switch" for value in selector(s)["buttons"]), "compact chooser should contain only clickable space rows"
        assert selector(s)["field"] == "list"
        tap(s, 15)
        assert selector(s)["field"] == "query"
        tap(s, 15)
        assert selector(s)["field"] == "list", "Tab must not stop at removed footer actions"
        capture(s, "spaces-drawer.png")
        tap(s, 1)
        assert not selector(s)["sidebar_visible"] and s.status()["live"]
        s.ctl("dispatch", "hyprspace:sidebar", "on")
        assert selector(s)["sidebar_visible"]
        s.ctl("dispatch", "hyprspace:sidebar", "on")
        assert selector(s)["sidebar_visible"]
        s.ctl("dispatch", "hyprspace:sidebar", "off")
        wait_for(lambda: not selector(s)["sidebar_visible"] and s.status()["live"])
        s.check("space drawer stays hidden until opened inside HyperSpace; Ctrl+Space, header and dispatcher preserve the workspace canvas")
        height = selector(s)["toolbar_height"]
        assert selector(s)["toolbar_visible"]
        s.key(44, 1)
        wait_for(lambda: s.status()["zoom"]["held"])
        assert not selector(s)["toolbar_visible"] and not selector(s)["buttons"]
        assert selector(s)["toolbar_height"] == height
        s.key(44, 0)
        wait_for(lambda: not s.status()["zoom"]["held"] and selector(s)["toolbar_visible"])
        assert selector(s)["layout"][s.names[0]] == geometry
        s.check("inspection hides space controls and release restores the same full-width canvas")
        if check_drag:
            toolbar = next(value["box"] for value in selector(s)["buttons"] if value["action"] == "sidebar" and value["monitor"] == s.names[0])
            s.drag(s.preview_point("hs-A"), (toolbar["x"] + toolbar["w"] / 2, toolbar["y"] + toolbar["h"] / 2), True)
            assert not s.status()["dragging"] and not selector(s)["active"]
            s.check("releasing a window gesture over the space rail completes its captured drag without opening a dialog")
        button(s, "create")
        assert selector(s)["mode"] == "create"
        s.run("wtype", "Hobby æøå")
        wait_for(lambda: selector(s)["input"] == "Hobby æøå")
        assert selector(s)["input_cursor"] == len("Hobby æøå".encode())
        tap(s, 15)
        assert selector(s)["edit_field"] == "icon"
        tap(s, 106)
        assert selector(s)["icon"] == "graduation-cap"
        tap(s, 15)
        assert selector(s)["edit_field"] == "color"
        for _ in range(4):
            tap(s, 106)
        assert selector(s)["color"] == "#f38ba8" and selector(s)["input"] == "Hobby æøå"
        capture(s, "spaces-create-unicode.png")
        save(s)
        first = wait_for(lambda: next((space["id"] for space in model(s)["spaces"] if space["name"] == "Hobby æøå"), None))
        wait_for(lambda: model(s)["active"] == first and all_outputs(s, first) and s.status()["live"] and not selector(s)["active"])
        assert all(owner(s, title) == baseline for title in ("hs-A", "hs-B", "hs-C"))
        appearance = next(space for space in model(s)["spaces"] if space["id"] == first)
        assert appearance["icon"] == "graduation-cap" and appearance["color"] == "#f38ba8"
        assert selector(s)["empty_space"]
        summary = next(space for space in selector(s)["spaces"] if space["id"] == first)
        assert summary["occupied_workspaces"] == 0 and summary["windows"] == 0
        assert any(value["action"] == "sidebar" for value in selector(s)["buttons"])
        capture(s, "spaces-empty.png")
        button(s, "enter")
        wait_for(lambda: not s.status()["live"])
        assert model(s)["active"] == first and all_outputs(s, first)
        opened(s, wait_for)
        s.key(125, 1)
        select_space(s, wait_for, baseline)
        s.key(125, 0)
        select_space(s, wait_for, first)

        button(s, "rename")
        assert selector(s)["mode"] == "rename" and selector(s)["input"] == "Hobby æøå"
        assert selector(s)["input_mode"] == "normal"
        tap(s, 23)  # i enters Name Insert.
        s.run("wtype", "Study æøå")
        assert selector(s)["input"] == "Study æøå"
        tap(s, 14)
        assert selector(s)["input"] == "Study æø"
        s.run("wtype", "å")
        button(s, "icon", "book-open")
        button(s, "color", "#cba6f7")
        save(s)
        wait_for(lambda: any(space["id"] == first and space["name"] == "Study æøå" for space in model(s)["spaces"]))
        assert s.status()["live"] and model(s)["active"] == first
        appearance = next(space for space in model(s)["spaces"] if space["id"] == first)
        assert appearance["icon"] == "book-open" and appearance["color"] == "#cba6f7"
        dismiss(s)
        s.check("create and rename support Unicode names and persistent Lucide icon/color choices through keyboard and pointer input")

        button(s, "create")
        s.run("wtype", "Work selector")
        button(s, "icon", "briefcase")
        save(s)
        second = wait_for(lambda: next((space["id"] for space in model(s)["spaces"] if space["name"] == "Work selector"), None))
        wait_for(lambda: model(s)["active"] == second and s.status()["live"])
        button(s, "sidebar")
        tap(s, 103)
        assert selector(s)["destination"] == first
        tap(s, 60)  # F2 edits the highlighted inactive space.
        assert selector(s)["mode"] == "rename" and selector(s)["source"] == first and model(s)["active"] == second
        assert selector(s)["icon"] == "book-open" and selector(s)["color"] == "#cba6f7"
        button(s, "icon", "graduation-cap")
        button(s, "color", "#f38ba8")
        save(s)
        wait_for(lambda: selector(s)["mode"] == "sidebar")
        first_appearance = next(space for space in model(s)["spaces"] if space["id"] == first)
        second_appearance = next(space for space in model(s)["spaces"] if space["id"] == second)
        assert first_appearance["icon"] == "graduation-cap" and first_appearance["color"] == "#f38ba8"
        assert second_appearance["icon"] == "briefcase" and second_appearance["color"] == "#89b4fa" and model(s)["active"] == second
        dismiss(s)
        button(s, "sidebar")
        tap(s, 103)
        tap(s, 111)  # Delete previews removing the highlighted space.
        assert selector(s)["mode"] == "remove" and selector(s)["source"] == first and model(s)["active"] == second
        tap(s, 1)
        s.check("F2 and Delete act on the highlighted inactive space, preserving the active space and its appearance")
        select_space(s, wait_for, first)
        button(s, "sidebar")
        selected_workspace = s.status()["target"]["workspace"]
        tap(s, 108)
        assert selector(s)["destination"] == second
        assert s.status()["target"]["workspace"] == selected_workspace
        tap(s, 28)
        wait_for(lambda: model(s)["active"] == second and all_outputs(s, second))
        assert not selector(s)["sidebar_visible"] and s.status()["live"]
        button(s, "sidebar")
        tap(s, 103)
        assert selector(s)["destination"] == first
        tap(s, 28)
        wait_for(lambda: model(s)["active"] == first and all_outputs(s, first))
        s.check("drawer arrows select spaces without retargeting the canvas; Enter switches every output, closes the drawer and keeps HyperSpace open")
        s.key(44, 1)
        wait_for(lambda: s.status()["zoom"]["held"])
        assert not selector(s)["toolbar_visible"]
        s.key(29, 1)
        tap(s, 109)
        s.key(29, 0)
        wait_for(lambda: model(s)["active"] == second and all_outputs(s, second) and len(s.status()["views"]) == len(s.names))
        s.key(44, 0)
        assert selector(s)["toolbar_visible"]
        s.key(29, 1)
        tap(s, 104)
        s.key(29, 0)
        wait_for(lambda: model(s)["active"] == first and all_outputs(s, first) and s.status()["keyboard_owned"])
        s.check("rail and Ctrl+Page Up/Down switch spaces across the same outputs, including while inspection hides the chrome")

        select_space(s, wait_for, baseline)
        s.move(s.preview_point("hs-A"))
        wait_for(lambda: s.status()["target"]["window"] == s.windows()["hs-A"]["address"])
        tap(s, 57)
        tap(s, 47)  # Space v: switch to the selected window in the board.
        wait_for(lambda: s.status()["window_board"]["active"])
        assert s.status()["window_board"]["selected_window"] == s.windows()["hs-A"]["address"]
        before_other = {title: s.windows()[title]["workspace"]["id"] for title in ("hs-B", "hs-C")}
        commands = {command["key"]: command["label"] for command in s.status()["input"]["commands"]}
        assert commands["m"] == "Move selected window to space" and commands["M"] == "Move workspace to space"
        tap(s, 57)
        tap(s, 50)  # Space m from the board defaults to this one window.
        assert selector(s)["mode"] == "move" and selector(s)["move_window"]
        button(s, "destination", first)
        capture(s, "spaces-move-board-window.png")
        button(s, "confirm")
        wait_for(lambda: owner(s, "hs-A") == first)
        assert all(owner(s, title) == baseline and s.windows()[title]["workspace"]["id"] == before_other[title] for title in before_other)
        assert s.status()["window_board"]["active"] and not selector(s)["active"]
        tap(s, 57)
        tap(s, 17)  # Space w restores the workspace grid.
        wait_for(lambda: not s.status()["window_board"]["active"])
        assert model(s)["active"] == baseline and s.status()["live"]
        s.check("Space m from the window board defaults to Selected window and leaves other windows/workspaces untouched")

        s.move(s.preview_point("hs-B"))
        wait_for(lambda: s.status()["target"]["window"] == s.windows()["hs-B"]["address"])
        workspace = s.windows()["hs-B"]["workspace"]["id"]
        logical = next(value["id"] for space in model(s)["spaces"] for value in space["workspaces"] if value["native_id"] == workspace)
        contents = {window["address"] for window in s.windows().values() if window["workspace"]["id"] == workspace}
        assert len(contents) == 2
        other_windows = {window["address"]: window["workspace"]["id"] for window in s.windows().values() if window["address"] not in contents}
        button(s, "move")
        assert not selector(s)["move_window"], "grid context-menu default must be Whole workspace, even over a window preview"
        button(s, "window")
        assert selector(s)["move_window"], "explicit single-window toggle is still available from the grid"
        button(s, "cancel")
        dismiss(s)
        commands = {command["key"]: command["label"] for command in s.status()["input"]["commands"]}
        assert commands["m"] == "Move selected window to space" and commands["M"] == "Move workspace to space"
        tap(s, 57)
        s.key(42, 1)
        tap(s, 50)  # Space Shift+M always uses the selected workspace.
        s.key(42, 0)
        assert selector(s)["mode"] == "move" and not selector(s)["move_window"]
        button(s, "destination", second)
        capture(s, "spaces-move-workspace.png")
        button(s, "confirm")
        wait_for(lambda: owner(s, "hs-B") == second and owner(s, "hs-C") == second)
        assert not selector(s)["active"]
        moved = next(value for space in model(s)["spaces"] if space["id"] == second for value in space["workspaces"] if value["id"] == logical)
        assert moved["native_id"] == workspace
        assert {window["address"] for window in s.windows().values() if window["workspace"]["id"] == workspace} == contents
        assert all(window["workspace"]["id"] == other_windows[window["address"]] for window in s.windows().values() if window["address"] in other_windows)
        assert model(s)["active"] == baseline and all_outputs(s, baseline) and s.status()["live"]
        s.check("grid Space Shift+M moves the whole workspace with stable logical/native identity across three outputs; the menu retains an explicit window-transfer toggle")

        select_space(s, wait_for, first)
        button(s, "remove")
        button(s, "destination", second)
        before = selector(s)
        assert before["mode"] == "remove" and before["source"] == first and before["destination"] == second
        assert before["preview"]["total"] >= 1 and before["preview"]["windows"] >= 1
        assert all("new_number" in item and "new_name" in item for item in before["preview"]["workspaces"])
        capture(s, "spaces-delete-preview.png")
        button(s, "confirm")
        wait_for(lambda: all(space["id"] != first for space in model(s)["spaces"]) and model(s)["active"] == second)
        assert owner(s, "hs-A") == second and s.status()["live"]
        s.check("Remove shows the destination and complete transfer preview, then moves populated workspaces and switches to that destination")

        searchable_icons(s, wait_for)
        reorder_spaces(s, wait_for)
        s.close()
        monitor_name = s.names[0]
        s.ctl("keyword", "monitor", f'{monitor_name},480x640@60,{original_monitor["x"]}x{original_monitor["y"]},1')
        resized = True
        try:
            wait_for(lambda: next(value for value in s.data("monitors") if value["name"] == monitor_name)["width"] == 480)
        except AssertionError:
            (s.root / "space-selector-resize-timeout.json").write_text(json.dumps({"monitors": s.data("monitors"), "configerrors": s.ctl("configerrors")}, indent=2))
            raise
        opened(s, wait_for)
        button(s, "pick")
        panel = selector(s)["panel"]
        for value in selector(s)["buttons"]:
            if value["monitor"] != monitor_name:
                continue
            box = value["box"]
            assert box["w"] > 0 and box["h"] > 0
            assert panel["x"] <= box["x"] and panel["y"] <= box["y"]
            assert box["x"] + box["w"] <= panel["x"] + panel["w"] + .01
            assert box["y"] + box["h"] <= panel["y"] + panel["h"] + .01
        capture(s, "spaces-compact-manage.png")
        button(s, "cancel")
        assert selector(s)["sidebar_visible"] and s.status()["live"]
        dismiss(s)
        button(s, "create")
        panel = selector(s)["panel"]
        palette = [value for value in selector(s)["buttons"] if value["monitor"] == monitor_name and value["action"] in ("icon", "color")]
        assert 1 <= sum(value["action"] == "icon" for value in palette) <= 24 and sum(value["action"] == "color" for value in palette) == 8
        assert selector(s)["icon_matches"] == 48
        assert all(panel["x"] <= value["box"]["x"] and value["box"]["x"] + value["box"]["w"] <= panel["x"] + panel["w"]
                   and panel["y"] <= value["box"]["y"] and value["box"]["y"] + value["box"]["h"] <= panel["y"] + panel["h"] for value in palette)
        capture(s, "spaces-compact-appearance.png")
        s.close()
        s.ctl("keyword", "monitor", f'{monitor_name},480x400@60,{original_monitor["x"]}x{original_monitor["y"]},1')
        wait_for(lambda: next(value for value in s.data("monitors") if value["name"] == monitor_name)["height"] == 400)
        opened(s, wait_for)
        button(s, "create")
        panel = selector(s)["panel"]
        search = selector(s)["search_box"]
        grid = selector(s)["icon_grid"]
        assert search["y"] + search["h"] <= grid["y"]
        for row in selector(s)["buttons"]:
            if row["monitor"] != monitor_name:
                continue
            box = row["box"]
            assert panel["x"] <= box["x"] and panel["y"] <= box["y"]
            assert box["x"] + box["w"] <= panel["x"] + panel["w"] + .01
            assert box["y"] + box["h"] <= panel["y"] + panel["h"] + .01
        tap(s, 15)
        s.run("wtype", "/school")
        wait_for(lambda: selector(s)["icon_matches"] == 1)
        capture(s, "icons-search-480x400.png")
        dismiss(s)
        s.check("compact displays expose the complete management controls with every button inside its panel")
    except BaseException:
        try:
            (s.root / "space-selector-failure.json").write_text(json.dumps(s.status(), indent=2, ensure_ascii=False))
            capture(s, "space-selector-failure.png")
        except Exception:
            pass
        raise
    finally:
        for code in (29, 42, 44, 57, 104, 109, 125):
            s.key(code, 0)
        s.close()
        if resized:
            s.ctl("keyword", "monitor", f'{s.names[0]},{original_monitor["width"]}x{original_monitor["height"]}@60,'
                  f'{original_monitor["x"]}x{original_monitor["y"]},{original_monitor["scale"]}')
        dispatch(s, {"action": "switch", "id": baseline})
        for space in list(model(s)["spaces"]):
            if space["id"] not in original:
                dispatch(s, {"action": "delete", "id": space["id"], "target": baseline})
        s.ctl("reload")
