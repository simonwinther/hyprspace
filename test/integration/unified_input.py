"""Keyboard-only Normal/Insert navigation in a disposable compositor."""

import json
import subprocess
import time

from space_selector import capture, dispatch, model, opened, save, selector, tap
from spaces import all_outputs


def input_state(s):
    return s.status()["input"]


def mode(s, wait_for, context, editing="normal", field=None):
    return wait_for(lambda: (value := input_state(s))["context"] == context
                    and value["mode"] == editing and (field is None or value["field"] == field) and value)


def chord(s, modifier, key):
    s.key(modifier, 1)
    tap(s, key)
    s.key(modifier, 0)


def leader(s, key):
    tap(s, 57)
    assert input_state(s)["prefix"] == "space", input_state(s)
    if key == "?":
        s.run("wtype", "?")
    else:
        tap(s, key)


def foreground_handoff(s, wait_for, name, prefix=57):
    # Only the key lease/prefix may reset; navigation parents and edits survive.
    tap(s, prefix)
    assert input_state(s)["prefix"] == ("g" if prefix == 34 else "space")
    entry = s.root / name
    foreground = s.spawn(["python3", str(s.artifact("layer.py")), str(entry)])
    try:
        wait_for(lambda: s.layer("hs-foreground") and not s.status()["keyboard_owned"])
        # Native focus changes before the next render/input ownership sync.
        wait_for(lambda: input_state(s)["prefix"] == "")
        s.run("wtype", "native")
        wait_for(lambda: entry.exists() and entry.read_text() == "native")
    finally:
        if foreground.poll() is None:
            foreground.terminate()
        foreground.wait(timeout=3)
        wait_for(lambda: s.status()["keyboard_owned"])
    assert input_state(s)["prefix"] == "", input_state(s)


def add_space(s, wait_for, name):
    leader(s, 30)  # Space a
    mode(s, wait_for, "space-editor", "insert", "name")
    assert selector(s)["mode"] == "create"
    s.run("wtype", name)
    save(s)
    identity = wait_for(lambda: next((space["id"] for space in model(s)["spaces"] if space["name"] == name), None))
    wait_for(lambda: model(s)["active"] == identity and all_outputs(s, identity))
    mode(s, wait_for, "workspaces")
    return identity


def retired_navigation_is_inert(s):
    """Removed visit shortcuts must not change any current navigation layer."""
    def snapshot():
        status = s.status()
        board = status["window_board"]
        chooser = status["space_selector"]
        return {
            "space": status["spaces"]["active"],
            "outputs": {monitor["name"]: monitor["activeWorkspace"]["id"] for monitor in s.data("monitors")},
            "target": status.get("target"),
            "input": {key: status["input"][key] for key in ("context", "mode", "field", "parents")},
            "board": {key: board[key] for key in ("active", "query", "grouping", "sort", "scroll", "selected_window")},
            "chooser": {key: chooser[key] for key in ("active", "mode", "query", "destination", "field")},
        }

    assert "history" not in s.status()
    assert not any(command["key"] == "h" for command in input_state(s)["commands"])
    before = snapshot()
    for code in (24, 23):  # Physical Ctrl+O / Ctrl+I; Ctrl+I must not become Tab.
        chord(s, 29, code)
        assert snapshot() == before
    leader(s, 35)  # An unbound Space h clears its prefix without opening another layer.
    assert input_state(s)["prefix"] == "" and snapshot() == before


def run(s, wait_for):
    s.setup("dwindle", 0, 1)
    baseline = model(s)["active"]
    original = {space["id"] for space in model(s)["spaces"]}
    s.ctl("keyword", "plugin:hyprspace:overview:font", "Sans 12")
    try:
        for command in ("navigate", "history"):
            attempt = subprocess.run(["hyprctl", "dispatch", "hyprspace:" + command], env=s.env,
                                     capture_output=True, text=True, timeout=10)
            output = attempt.stdout + attempt.stderr
            assert "invalid dispatcher" in output.lower(), output
        # More than one tile makes gg/G assertions observable, including empty
        # persistent workspaces rather than accepting an unchanged single tile.
        for number in (21, 22):
            s.ctl("keyword", "workspace", f"{number},monitor:{s.names[0]},persistent:true")
            s.ctl("dispatch", "focusmonitor", s.names[0])
            s.ctl("dispatch", "workspace", str(number))
        s.ctl("dispatch", "workspace", "11")
        s.move(s.point(s.windows()["hs-A"]))
        opened(s, wait_for)
        mode(s, wait_for, "workspaces")
        tap(s, 34)
        assert input_state(s)["prefix"] == "g"
        tap(s, 34)
        assert s.status()["target"]["workspace"] == 11
        chord(s, 42, 34)  # Actual modifier-independent g + Shift.
        assert s.status()["target"]["workspace"] == 22
        tap(s, 34)
        tap(s, 34)
        assert s.status()["target"]["workspace"] == 11 and s.status()["live"]
        s.check("Normal gg/G select first/last observable tiles through real Shift key events without entering a different view")

        leader(s, "?")
        mode(s, wait_for, "help")
        s.run("wtype", "/")
        mode(s, wait_for, "help", "insert")
        s.run("wtype", "hjkl space mM")
        assert s.status()["help"]["query"] == "hjkl space mM"
        tap(s, 28)
        mode(s, wait_for, "help")
        assert s.status()["help"]["query"] == "hjkl space mM"
        tap(s, 1)
        mode(s, wait_for, "workspaces")
        s.check("Space ? opens help; printable navigation letters are literal in Insert and Enter returns to Normal with the query retained")

        s.run("wtype", "/")
        mode(s, wait_for, "windows", "insert")
        s.run("wtype", "hjkl space mM")
        assert s.status()["window_board"]["query"] == "hjkl space mM"
        tap(s, 28)
        mode(s, wait_for, "windows")
        assert s.status()["window_board"]["query"] == "hjkl space mM" and s.status()["live"]
        tap(s, 28)  # No results must not commit a stale hidden target.
        assert s.status()["live"]
        tap(s, 23)  # i resumes the same query.
        mode(s, wait_for, "windows", "insert")
        chord(s, 29, 22)  # Ctrl+U
        s.run("wtype", "hs-")
        tap(s, 1)
        mode(s, wait_for, "windows")
        assert s.status()["window_board"]["query"] == "hs-"
        leader(s, 34)  # Space g grouping
        leader(s, 19)  # Space r recent order
        board = s.status()["window_board"]
        parent = {key: board[key] for key in ("query", "grouping", "sort", "selected_window")}
        parents = input_state(s)["parents"]
        retired_navigation_is_inert(s)
        foreground_handoff(s, wait_for, "unified-board-foreground")
        mode(s, wait_for, "windows")
        assert {key: s.status()["window_board"][key] for key in parent} == parent
        assert input_state(s)["parents"] == parents
        tap(s, 47)  # A suffix after handoff must not complete the old leader.
        assert input_state(s)["prefix"] == "" and input_state(s)["parents"] == parents
        assert {key: s.status()["window_board"][key] for key in parent} == parent
        s.check("foreground input handoff clears a pending leader while retaining the filtered board, selection and navigation parents")
        leader(s, 31)  # Space s
        mode(s, wait_for, "spaces")
        tap(s, 1)
        mode(s, wait_for, "windows")
        assert {key: s.status()["window_board"][key] for key in parent} == parent
        s.run("wtype", "/")
        mode(s, wait_for, "windows", "insert")
        chord(s, 29, 22)
        s.run("wtype", "hs-A")
        tap(s, 28)
        mode(s, wait_for, "windows")
        tap(s, 1)
        mode(s, wait_for, "windows")
        assert {key: s.status()["window_board"][key] for key in parent} == parent
        tap(s, 1)
        mode(s, wait_for, "workspaces")
        s.check("window search Enter/Escape finish Insert without committing; i resumes, and Back restores the prior filtered/grouped board before the grid")

        first = add_space(s, wait_for, "Keyboard School")
        second = add_space(s, wait_for, "Keyboard Work")
        retired_navigation_is_inert(s)
        assert model(s)["active"] == second and all_outputs(s, second)
        leader(s, 31)
        mode(s, wait_for, "spaces")
        s.run("wtype", "/")
        mode(s, wait_for, "spaces", "insert")
        s.run("wtype", "Keyboard School")
        tap(s, 28)
        mode(s, wait_for, "spaces")
        assert selector(s)["destination"] == first
        retired_navigation_is_inert(s)
        capture(s, "unified-space-filter.png")
        tap(s, 28)
        wait_for(lambda: model(s)["active"] == first and all_outputs(s, first))
        mode(s, wait_for, "workspaces")
        s.check("removed Space h and Ctrl+O/Ctrl+I leave grid, filtered Board and chooser unchanged; the searchable chooser still switches every output")

        leader(s, 31)
        mode(s, wait_for, "spaces")
        s.run("wtype", "/")
        s.run("wtype", "Keyboard")
        tap(s, 28)
        mode(s, wait_for, "spaces")
        tap(s, 36)  # j highlights the inactive Work space.
        assert selector(s)["destination"] == second
        chooser = {"query": selector(s)["query"], "destination": selector(s)["destination"]}
        leader(s, 18)  # Space e
        mode(s, wait_for, "space-editor", "normal", "name")
        assert selector(s)["source"] == second and selector(s)["parent_depth"] >= 1
        tap(s, 23)
        mode(s, wait_for, "space-editor", "insert", "name")
        chord(s, 29, 30)
        s.run("wtype", "Keyboard Work hjkl space")
        tap(s, 28)
        mode(s, wait_for, "space-editor", "normal", "name")
        assert selector(s)["input"] == "Keyboard Work hjkl space"
        tap(s, 15)
        mode(s, wait_for, "space-editor", "normal", "icon")
        s.key(38, 1)  # Hold l across a focus change: its repeat must not edit Color.
        icon = selector(s)["icon"]
        tap(s, 15)
        mode(s, wait_for, "space-editor", "normal", "color")
        color = selector(s)["color"]
        time.sleep(.8)
        s.key(38, 0)
        assert selector(s)["icon"] == icon and selector(s)["color"] == color
        tap(s, 36)
        draft = {key: selector(s)[key] for key in ("input", "icon", "color")}
        s.run("wtype", "?")
        mode(s, wait_for, "help")
        chord(s, 29, 24)
        chord(s, 29, 23)
        assert input_state(s)["context"] == "help" and model(s)["active"] == first
        assert {key: selector(s)[key] for key in draft} == draft
        tap(s, 1)
        mode(s, wait_for, "space-editor", "normal", "color")
        assert {key: selector(s)[key] for key in draft} == draft
        capture(s, "unified-editor-normal.png")
        save(s)
        mode(s, wait_for, "spaces")
        assert {key: selector(s)[key] for key in chooser} == chooser
        updated = next(space for space in model(s)["spaces"] if space["id"] == second)
        assert updated["name"] == draft["input"] and updated["icon"] == draft["icon"] and updated["color"] == draft["color"]
        s.check("Space e edits the highlighted inactive space; hjkl changes appearance, held repeats cannot cross fields, and help/save restore the filtered chooser")

        leader(s, 18)
        mode(s, wait_for, "space-editor", "normal", "name")
        tap(s, 23)
        chord(s, 29, 30)
        s.run("wtype", "Unsaved draft hjkl")
        for code in (24, 57, 109):
            chord(s, 29, code)
            assert selector(s)["input"] == "Unsaved draft hjkl" and selector(s)["mode"] == "rename"
        tap(s, 1)
        mode(s, wait_for, "space-editor", "normal", "name")
        for code in (24, 57, 109):
            chord(s, 29, code)
            assert selector(s)["input"] == "Unsaved draft hjkl" and selector(s)["mode"] == "rename"
        saved_draft = {key: selector(s)[key] for key in ("input", "icon", "color", "source", "parent_depth", "field")}
        foreground_handoff(s, wait_for, "unified-draft-foreground", prefix=34)
        mode(s, wait_for, "space-editor", "normal", "name")
        assert {key: selector(s)[key] for key in saved_draft} == saved_draft
        tap(s, 1)
        mode(s, wait_for, "spaces")
        assert {key: selector(s)[key] for key in chooser} == chooser
        assert next(space["name"] for space in model(s)["spaces"] if space["id"] == second) == draft["input"]
        tap(s, 1)
        mode(s, wait_for, "workspaces")
        s.check("global chords and foreground handoff preserve form drafts and chooser parents; handoff clears prefixes, and Escape finishes Insert before returning")

        leader(s, 50)  # Space m
        mode(s, wait_for, "space-move")
        assert selector(s)["move_window"] and selector(s)["error"] == "Select a window to move."
        tap(s, 15)
        assert selector(s)["field"] == "type"
        tap(s, 15)
        assert selector(s)["field"] == "cancel"
        tap(s, 28)
        mode(s, wait_for, "workspaces")
        temporary = add_space(s, wait_for, "Keyboard Temporary")
        leader(s, 32)  # Space d
        mode(s, wait_for, "space-remove")
        assert selector(s)["source"] == temporary and selector(s)["preview"]["total"] >= 1
        tap(s, 36)  # baseline -> School destination
        assert selector(s)["destination"] == first
        tap(s, 28)
        wait_for(lambda: all(space["id"] != temporary for space in model(s)["spaces"]) and model(s)["active"] == first)
        mode(s, wait_for, "workspaces")
        s.check("Space m exposes keyboard-reachable transfer controls; Space d previews then transfers/removes into the chosen destination")
    except BaseException:
        (s.root / "unified-input-failure.json").write_text(json.dumps(s.status(), indent=2))
        capture(s, "unified-input-failure.png")
        raise
    finally:
        for code in (29, 42, 38, 57):
            s.key(code, 0)
        s.close()
        dispatch(s, {"action": "switch", "id": baseline})
        for space in list(model(s)["spaces"]):
            if space["id"] not in original:
                dispatch(s, {"action": "delete", "id": space["id"], "target": baseline})
        s.ctl("reload")
