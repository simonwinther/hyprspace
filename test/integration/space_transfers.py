"""Explicit window/workspace transfers in a disposable three-output compositor."""

import json
import time

from space_selector import button, capture, dispatch, model, opened, owner, selector, tap
from spaces import all_outputs


def chord(s, modifier, key):
    s.key(modifier, 1)
    try:
        tap(s, key)
    finally:
        s.key(modifier, 0)


def leader(s, key, shift=False):
    tap(s, 57)
    assert s.status()["input"]["prefix"] == "space"
    if shift:
        chord(s, 42, key)
    else:
        tap(s, key)


def placement(s):
    return {window["address"]: window["workspace"]["id"] for window in s.windows().values()}


def logical_workspace(s, native):
    return next(workspace["id"] for space in model(s)["spaces"]
                for workspace in space["workspaces"] if workspace["native_id"] == native)


def assert_commands(s):
    labels = {command["key"]: command["label"] for command in s.status()["input"]["commands"]}
    assert labels["m"] == "Move selected window to space", labels
    assert labels["M"] == "Move workspace to space", labels


def run(s, wait_for):
    baseline = model(s)["active"]
    original = {space["id"] for space in model(s)["spaces"]}
    destinations = []

    def reset(context="grid"):
        s.close()
        dispatch(s, {"action": "switch", "id": baseline})
        wait_for(lambda: model(s)["active"] == baseline and all_outputs(s, baseline))
        s.setup("dwindle", 0, 1)
        assert all(owner(s, title) == baseline for title in ("hs-A", "hs-B", "hs-C"))
        s.ctl("keyword", "plugin:hyprspace:follow_mouse", "true")
        opened(s, wait_for)
        s.move(s.preview_point("hs-B"))
        address = s.windows()["hs-B"]["address"]
        wait_for(lambda: s.status()["target"]["window"] == address)
        assert s.data("activewindow")["address"] == s.windows()["hs-A"]["address"]
        if context == "board":
            leader(s, 47)  # Space v
            wait_for(lambda: s.status()["window_board"]["active"])
            assert s.status()["window_board"]["selected_window"] == address
        assert_commands(s)
        return address

    def open_transfer(scope, expected_window=None, expected_workspace=None):
        leader(s, 50, shift=scope == "workspace")  # Physical Shift+M, not a text-only uppercase simulation.
        wait_for(lambda: selector(s)["mode"] == "move")
        value = selector(s)
        assert value["move_window"] == (scope == "window"), value
        assert value["transfer_scope"] == scope, value
        if expected_window is not None:
            assert value["selected_window"] == expected_window, value
        if expected_workspace is not None:
            assert value["selected_workspace"] == expected_workspace, value

    try:
        for name in ("Window destination", "Workspace destination"):
            dispatch(s, {"action": "create", "name": name})
            destinations.append(wait_for(lambda: next((space["id"] for space in model(s)["spaces"] if space["name"] == name), None)))

        for context in ("grid", "board"):
            for scope in ("window", "workspace"):
                selected = reset(context)
                native = s.windows()["hs-B"]["workspace"]["id"]
                identity = logical_workspace(s, native)
                contents = {window["address"] for window in s.windows().values() if window["workspace"]["id"] == native}
                assert len(contents) == 2
                before = placement(s)
                shape = {window["address"]: (window["at"], window["size"], window["floating"], window["fullscreen"])
                         for window in s.windows().values() if window["address"] in contents}
                open_transfer(scope, selected, native)
                # The explicit type switch remains keyboard accessible in both contexts.
                tap(s, 15)
                assert selector(s)["field"] == "type"
                tap(s, 106 if scope == "window" else 105)
                assert selector(s)["move_window"] == (scope != "window")
                tap(s, 105 if scope == "window" else 106)
                assert selector(s)["move_window"] == (scope == "window")
                target = destinations[0 if scope == "window" else 1]
                button(s, "destination", target)
                capture(s, f"transfer-{context}-{scope}.png")
                tap(s, 28)
                wait_for(lambda: owner(s, "hs-B") == target and not selector(s)["active"])
                if scope == "window":
                    assert owner(s, "hs-C") == baseline
                    assert all(window["workspace"]["id"] == before[window["address"]]
                               for window in s.windows().values() if window["address"] != selected)
                else:
                    assert owner(s, "hs-C") == target
                    moved = next(workspace for space in model(s)["spaces"] if space["id"] == target
                                 for workspace in space["workspaces"] if workspace["id"] == identity)
                    assert moved["native_id"] == native
                    assert {window["address"] for window in s.windows().values() if window["workspace"]["id"] == native} == contents
                    assert {window["address"]: (window["at"], window["size"], window["floating"], window["fullscreen"])
                            for window in s.windows().values() if window["address"] in contents} == shape
                    assert placement(s) == before
                assert model(s)["active"] == baseline and all_outputs(s, baseline) and len(s.data("monitors")) == 3
                assert s.status()["window_board"]["active"] == (context == "board")
                s.check(f"{context}: Space {'Shift+M' if scope == 'workspace' else 'm'} transfers only the captured {scope}, preserving unrelated windows and all three active outputs")

        # A workspace-only selection must not borrow the underlying desktop focus.
        reset()
        s.close()
        s.ctl("keyword", "workspace", f"21,monitor:{s.names[0]},persistent:true")
        s.ctl("dispatch", "focusmonitor", s.names[0])
        s.ctl("dispatch", "workspace", "21")
        empty = s.data("activeworkspace")["id"]
        empty_identity = logical_workspace(s, empty)
        monitor = next(value for value in s.data("monitors") if value["name"] == s.names[0])
        s.move((monitor["x"] + monitor["reserved"][0] + 30, monitor["y"] + monitor["reserved"][1] + 30))
        opened(s, wait_for)
        wait_for(lambda: s.status()["target"]["workspace"] == empty)
        before = placement(s)
        ownership = {workspace["id"]: space["id"] for space in model(s)["spaces"] for workspace in space["workspaces"]}
        open_transfer("window", expected_workspace=empty)
        assert selector(s)["error"] == "Select a window to move."
        assert selector(s)["selected_window"] in ("0x0", None)
        tap(s, 28)
        assert selector(s)["active"] and selector(s)["move_window"] and selector(s)["error"]
        assert placement(s) == before
        assert {workspace["id"]: space["id"] for space in model(s)["spaces"] for workspace in space["workspaces"]} == ownership
        tap(s, 15)
        assert selector(s)["field"] == "type"
        tap(s, 106)  # Explicitly choose Whole workspace even without a selected window.
        assert not selector(s)["move_window"]
        tap(s, 105)  # Returning to Selected window must show the error, never fall back.
        assert selector(s)["move_window"] and selector(s)["error"]
        tap(s, 1)
        open_transfer("workspace", expected_workspace=empty)
        destination = selector(s)["destination"]
        tap(s, 28)
        wait_for(lambda: not selector(s)["active"])
        assert next(workspace for space in model(s)["spaces"] if space["id"] == destination
                    for workspace in space["workspaces"] if workspace["id"] == empty_identity)["native_id"] == empty
        assert placement(s) == before and all_outputs(s, baseline)
        s.check("empty grid selection rejects window transfer without falling back; explicit scope controls and Shift+M still allow an empty workspace transfer")

        for stale in ("closed", "other-workspace", "other-space"):
            selected = reset()
            source = s.windows()["hs-B"]["workspace"]["id"]
            open_transfer("window", selected, source)
            button(s, "destination", destinations[1])
            if stale == "closed":
                s.ctl("dispatch", "closewindow", "address:" + selected)
                wait_for(lambda: "hs-B" not in s.windows())
            elif stale == "other-workspace":
                s.ctl("dispatch", "movetoworkspacesilent", "11,address:" + selected)
                wait_for(lambda: s.windows()["hs-B"]["workspace"]["id"] == s.windows()["hs-A"]["workspace"]["id"])
            else:
                s.ctl("dispatch", "hyprspace:movetospace", json.dumps({"target": "window", "space_id": destinations[0], "window": selected}))
                wait_for(lambda: owner(s, "hs-B") == destinations[0])
            before = placement(s)
            tap(s, 28)
            wait_for(lambda: selector(s)["error"] == "The selected window is no longer available.")
            assert selector(s)["active"] and selector(s)["move_window"] and placement(s) == before
            assert owner(s, "hs-C") == baseline and model(s)["active"] == baseline and all_outputs(s, baseline)
            capture(s, f"transfer-stale-{stale}.png")
            tap(s, 1)
            s.check(f"a selected window {stale} after opening the transfer picker cannot move another window or its previous workspace")

        # Keep a real, nonzero scroll offset and a selected card through cancellation.
        reset()
        s.close()
        titles = [f"hs-Transfer scroll {i:02}" for i in range(21)]
        s.spawn(["python3", str(s.client), *titles])
        wait_for(lambda: len(s.windows()) == 24)
        opened(s, wait_for)
        s.run("wtype", "/hs-")
        tap(s, 28)
        wait_for(lambda: s.status()["window_board"]["active"] and not s.status()["window_board"]["search_focused"])
        leader(s, 34)
        leader(s, 19)
        board = s.status()["window_board"]
        host = next(monitor for monitor in s.data("monitors") if monitor["name"] == board["monitor"])
        viewport = board["viewport"]
        s.move((host["x"] + viewport["x"] + viewport["w"] / 2,
                host["y"] + viewport["y"] + viewport["h"] / 2))
        tap(s, 102)
        time.sleep(.25)
        s.scroll(90, 0, source=2)
        wait_for(lambda: s.status()["window_board"]["scroll"] > 0)
        keys = ("query", "grouping", "sort", "scroll", "selected_window")
        before = {key: s.status()["window_board"][key] for key in keys}
        parents = s.status()["input"]["parents"]
        for scope in ("window", "workspace"):
            open_transfer(scope)
            tap(s, 1)
            wait_for(lambda: not selector(s)["active"] and s.status()["window_board"]["active"])
            assert {key: s.status()["window_board"][key] for key in keys} == before
            assert s.status()["input"]["parents"] == parents
        leader(s, 17)  # Space w still returns to the workspace grid.
        wait_for(lambda: not s.status()["window_board"]["active"] and s.status()["live"])
        s.check("cancelling either transfer preserves filtered Board grouping, ordering, nonzero scroll, selection and parents; Space w still returns to workspaces")

        # No selected result may be replaced with a native focused window.
        s.run("wtype", "/no-such-transfer-window")
        tap(s, 28)
        wait_for(lambda: s.status()["window_board"]["count"] == 0)
        before = placement(s)
        open_transfer("window")
        assert selector(s)["error"] == "Select a window to move."
        tap(s, 28)
        assert selector(s)["active"] and selector(s)["move_window"] and placement(s) == before
        tap(s, 1)
        assert s.status()["window_board"]["query"] == "no-such-transfer-window"
        s.check("an empty filtered Board rejects window transfer without substituting native focus and restores its query on cancellation")
    except BaseException:
        (s.root / "space-transfer-failure.json").write_text(json.dumps(s.status(), indent=2))
        capture(s, "space-transfer-failure.png")
        raise
    finally:
        for code in (29, 42, 50, 57):
            s.key(code, 0)
        s.close()
        dispatch(s, {"action": "switch", "id": baseline})
        for space in list(model(s)["spaces"]):
            if space["id"] not in original:
                dispatch(s, {"action": "delete", "id": space["id"], "target": baseline})
        s.ctl("reload")
