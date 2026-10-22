"""Exercise the actual panel protocol against private native workspaces."""

import json

from spaces import all_outputs, command, create, local, metadata_events, owner, state, switch


def snapshot(s):
    return json.loads(s.request("spaces"))


def select(s, identity, monitor, workspace):
    return json.loads(s.request("workspace " + json.dumps({
        "space": identity, "monitor": monitor, "workspace": workspace,
    })))


def run(s, wait_for):
    metadata_events(s, wait_for)
    s.setup("dwindle", 0, 1)
    original = state(s)["active"]
    first = snapshot(s)
    assert first["schema"] == 1 and first["type"] == "spaces"
    assert first["enabled"] and first["active"] == original
    assert sum(ws["windows"] for ws in first["workspaces"]) == 3
    assert all(owner(s, ws["native_id"]) == original
               for ws in first["workspaces"] if ws["native_id"])
    assert len(first["monitors"]) == 3
    summary = next(value for value in first["spaces"] if value["id"] == original)
    assert summary["icon"] == "grid" and summary["color"] == "#89b4fa"
    command(s, "update", id=original, icon="graduation-cap", color="#ED8796")
    changed = next(value for value in snapshot(s)["spaces"] if value["id"] == original)
    assert changed["icon"] == "graduation-cap" and changed["color"] == "#ed8796"
    command(s, "update", id=original, icon="grid", color="#89b4fa")
    s.check("panel snapshot projects active-space occupancy and each monitor's local selection")

    # A same-monitor active-slot click emits no native workspace event. It must
    # still replace a different hover target before toggle-dismiss commits it.
    s.ctl("keyword", "workspace", f"21,monitor:{s.names[0]},persistent:true")
    s.ctl("dispatch", "movetoworkspacesilent", "21,address:" + s.windows()["hs-B"]["address"])
    s.ctl("dispatch", "focuswindow", "address:" + s.windows()["hs-A"]["address"])
    s.ctl("dispatch", "hyprspace:overview", "on")
    wait_for(lambda: s.status()["live"] and s.status()["keyboard_owned"])
    s.move(s.preview_point("hs-B"))
    wait_for(lambda: s.status()["target"]["workspace"] == 21)
    assert s.data("activeworkspace")["id"] == 11
    assert select(s, original, s.names[0], "11")["success"]
    wait_for(lambda: s.status()["target"]["workspace"] == 11)
    s.ctl("dispatch", "hyprspace:overview")
    wait_for(lambda: not s.status()["views"])
    assert s.data("activeworkspace")["id"] == 11
    s.check("clicking an already-active panel slot replaces a different hover target without toggling away on overview dismissal")

    hobby = create(s, wait_for, "Panel independent space")
    empty = snapshot(s)
    old_ids = {ws["native_id"] for ws in first["workspaces"] if ws["native_id"]}
    assert empty["active"] == hobby
    assert all(not ws["occupied"] and not ws["windows"] for ws in empty["workspaces"])
    assert not old_ids.intersection(ws["native_id"] for ws in empty["workspaces"])
    assert next(item for item in empty["spaces"] if item["id"] == original)["windows"] == 3
    assert all_outputs(s, hobby)
    s.check("a new space publishes empty local slots without exposing inactive workspace IDs or occupancy")

    s.ctl("keyword", "binds:workspace_back_and_forth", "true")
    for number in (4, 1, 4):
        assert select(s, hobby, s.names[0], str(number)) == {"success": True, "error": ""}
        destination = local(s, hobby, number)["native_id"]
        wait_for(lambda: s.data("activeworkspace")["id"] == destination)
        # Repeating an active panel click must not trigger back-and-forth.
        assert select(s, hobby, s.names[0], str(number))["success"]
        assert s.data("activeworkspace")["id"] == destination
        assert owner(s, destination) == hobby
    s.check("panel clicks resolve local numbers and repeated clicks remain idempotent with native back-and-forth enabled")

    before = state(s)
    selections = [(m["name"], m["activeWorkspace"]["id"]) for m in s.data("monitors")]
    focused = s.data("activeworkspace")["id"]
    bad_requests = (
        {"space": original, "monitor": s.names[1], "workspace": "99"},
        {"space": hobby, "monitor": "disconnected-monitor", "workspace": "99"},
        {"space": hobby, "monitor": s.names[1], "workspace": "name:hyprspace:private"},
        {"space": hobby, "monitor": s.names[1], "workspace": "1\n2"},
        {"space": hobby, "monitor": s.names[1], "workspace": 99},
        {},
    )
    for request in bad_requests:
        assert not json.loads(s.request("workspace " + json.dumps(request)))["success"]
    assert state(s) == before
    assert s.data("activeworkspace")["id"] == focused
    assert selections == [(m["name"], m["activeWorkspace"]["id"]) for m in s.data("monitors")]
    s.check("stale-space clicks and malformed panel requests fail without focus, namespace or monitor-selection side effects")

    # This uses the same private IPC path as the installed desktop strip.
    s.ctl("keyword", "binds:workspace_back_and_forth", "false")
    s.ctl("dispatch", "hyprspace:overview", "on")
    wait_for(lambda: s.status()["live"] and s.status()["keyboard_owned"])
    assert select(s, hobby, s.names[0], "8")["success"]
    destination = local(s, hobby, 8)["native_id"]
    wait_for(lambda: s.status()["target"]["workspace"] == destination)
    s.close()
    assert s.data("activeworkspace")["id"] == destination
    switch(s, wait_for, original)
    restored = snapshot(s)
    assert restored["active"] == original
    assert sum(ws["windows"] for ws in restored["workspaces"]) == 3
    s.check("a panel selection updates the open overview target and survives dismissal; switching back restores the other space's occupancy")
