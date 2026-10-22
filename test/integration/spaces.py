"""Independent logical workspace sets in a disposable three-output compositor."""

import json
import shlex
import subprocess
from pathlib import Path


def state(s):
    return s.status()["spaces"]


def space(s, identity):
    return next(item for item in state(s)["spaces"] if item["id"] == identity)


def local(s, identity, number):
    return next((item for item in space(s, identity)["workspaces"]
                 if item["number"] == number and not item["special"]), None)


def owner(s, native_id):
    return next((item["id"] for item in state(s)["spaces"]
                 if any(ws["native_id"] == native_id for ws in item["workspaces"])), None)


def all_outputs(s, identity):
    ids = {ws["native_id"] for ws in space(s, identity)["workspaces"] if not ws["special"]}
    outputs = s.data("monitors")
    return bool(outputs) and all(m["activeWorkspace"]["id"] in ids for m in outputs)


def monitor_selections(s, identity):
    """Return logical UUIDs, since native numeric IDs change after a restart."""
    workspaces = {ws["native_id"]: ws["id"] for ws in space(s, identity)["workspaces"] if not ws["special"]}
    outputs = s.data("monitors")
    if any(m["activeWorkspace"]["id"] not in workspaces for m in outputs):
        return {}
    return {m["name"]: workspaces[m["activeWorkspace"]["id"]] for m in outputs}


def fresh_compositor(s, wait_for, persisted, expected):
    """Seed the saved model before the plugin sees a genuinely new compositor."""
    # Suite prepares private XDG roots before calling start(), so this override
    # seeds only the child session and shares the immutable binary snapshot.
    class SeededSuite(type(s)):
        def start(self):
            destination = Path(self.env["XDG_STATE_HOME"]) / "hyprspace/spaces.json"
            destination.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            destination.write_text(json.dumps(persisted))
            destination.chmod(0o600)
            super().start()

    fresh = SeededSuite(snapshot=s.snapshot, group="spaces-fresh-start")
    try:
        assert fresh.compositor_pid != s.compositor_pid
        assert fresh.env["HYPRLAND_INSTANCE_SIGNATURE"] != s.env["HYPRLAND_INSTANCE_SIGNATURE"]
        wait_for(lambda: state(fresh)["active"] == persisted["active"]
                 and monitor_selections(fresh, persisted["active"]) == expected)
        assert {item["id"]: item["name"] for item in state(fresh)["spaces"]} == {
            item["id"]: item["name"] for item in persisted["spaces"]}
        # Selection UUIDs survive while clients from the old session do not.
        assert not fresh.windows()
        active = persisted["active"]
        selected_number = next(ws["number"] for ws in space(fresh, active)["workspaces"]
                               if ws["id"] == expected[fresh.names[0]])
        fresh.ctl("dispatch", "focusmonitor", fresh.names[0])
        launched = spawn(fresh, wait_for, "hs-space-fresh-session", selected_number)
        assert owner(fresh, launched["workspace"]["id"]) == active
        assert monitor_selections(fresh, active) == expected
        fresh.check("fresh compositor restores the saved active space and each monitor's logical selection before new launches")
    except BaseException:
        print("Fresh compositor persistence artifacts:", fresh.root, flush=True)
        raise
    finally:
        fresh.finish()
    s.check("a new compositor restores saved non-Default space identities and exact per-output UUID selections")


def command(s, action, **values):
    result = s.ctl("dispatch", "hyprspace:space", json.dumps({"action": action, **values}))
    assert result == "ok", result


def switch(s, wait_for, identity):
    command(s, "switch", id=identity)
    wait_for(lambda: state(s)["active"] == identity and all_outputs(s, identity))


def create(s, wait_for, name):
    command(s, "create", name=name)
    identity = next(item["id"] for item in state(s)["spaces"] if item["name"] == name)
    wait_for(lambda: state(s)["active"] == identity and all_outputs(s, identity))
    return identity


def rejected(s, action, **values):
    try:
        output = s.run("hyprctl", "dispatch", "hyprspace:space", json.dumps({"action": action, **values}))
    except subprocess.CalledProcessError as error:
        output = error.output
    assert output != "ok", output


def spawn(s, wait_for, title, number):
    s.ctl("dispatch", "workspace", str(number))
    process = s.spawn(["python3", str(s.client), title])
    window = wait_for(lambda: s.windows().get(title))
    assert process.poll() is None
    return window


def move(s, **request):
    result = s.ctl("dispatch", "hyprspace:movetospace", json.dumps(request))
    assert result == "ok", result


def lua_workspace_actions(s, wait_for):
    """Run with a fresh LuaSuite: use the user's native key and bar actions."""
    original = state(s)["active"]
    s.ctl("eval", 'hl.dispatch(hl.dsp.focus({ workspace = "1" })); '
          'hs_before_focus = hl.dsp.focus({ workspace = "1" }); '
          'hs_before_move = hl.dsp.window.move({ workspace = "2", follow = false }); '
          'hl.bind("SUPER + code:10", hl.dsp.focus({ workspace = "1" }))')
    original_one = local(s, original, 1)["native_id"]
    request = json.dumps({"action": "create", "name": "Hobby native Lua"})
    s.ctl("eval", "local r = hl.plugin.hyprspace.space(" + json.dumps(request) + "); assert(r.ok, r.error)")
    hobby = next(value["id"] for value in state(s)["spaces"] if value["name"] == "Hobby native Lua")
    wait_for(lambda: state(s)["active"] == hobby and all_outputs(s, hobby))
    assert local(s, hobby, 1)["native_id"] != original_one

    # These closures were constructed before a second space existed. The
    # original pre-plugin Super+8 binding also has to resolve at invocation.
    for code, number in ((2, 1), (9, 8)):
        s.key(125, 1)
        try:
            s.key(code, 1)
            s.key(code, 0)
        finally:
            s.key(125, 0)
        wait_for(lambda: state(s)["active"] == hobby
                 and s.data("activeworkspace")["id"] == local(s, hobby, number)["native_id"])
    s.ctl("eval", "hl.dispatch(hs_before_focus)")
    assert s.data("activeworkspace")["id"] == local(s, hobby, 1)["native_id"]
    s.spawn(["python3", str(s.client), "hs-lua-space-window"])
    wait_for(lambda: "hs-lua-space-window" in s.windows())
    s.ctl("eval", "hl.dispatch(hs_before_move)")
    wait_for(lambda: s.windows()["hs-lua-space-window"]["workspace"]["id"] == local(s, hobby, 2)["native_id"])
    assert state(s)["active"] == hobby
    s.check("native Lua numeric keybindings and cached silent window moves retain independent local workspace numbers after creating a space")

    for number in (4, 1, 8):
        s.ctl("eval", 'hl.plugin.hyprspace.overview("on")')
        wait_for(lambda: s.status()["live"] and s.status()["keyboard_owned"])
        # A panel click moves the pointer from its opening position. Dismissing
        # afterwards must commit the new native destination, not the old tile.
        s.motion(5, 3)
        s.ctl("eval", 'hl.dispatch(hl.dsp.focus({ monitor = "' + s.names[0] + '" })); '
              'hl.dispatch(hl.dsp.focus({ workspace = "' + str(number) + '" }))')
        destination = local(s, hobby, number)["native_id"]
        wait_for(lambda: s.status()["target"]["workspace"] == destination)
        assert state(s)["active"] == hobby
        assert s.data("activeworkspace")["id"] == destination
        s.ctl("eval", "hl.plugin.hyprspace.overview()")
        wait_for(lambda: not s.status()["views"])
        assert s.data("activeworkspace")["id"] == destination
    s.check("Lua panel workspace actions update the overview selection and dismiss to their chosen local workspace instead of the previous tile")

    # The drawer belongs to the existing overview entry. Calling its Lua API
    # from the desktop must never create another entry or capture input.
    s.ctl("eval", 'local r = hl.plugin.hyprspace.sidebar("on"); '
          'assert(not r.ok and r.error:find("open overview"), r.error)')
    closed = s.status()
    assert not closed["live"] and not closed["views"]
    assert not closed["space_selector"]["sidebar_visible"]
    assert not closed["keyboard_owned"] and not closed["cursor_owned"]

    # Use the real, existing Lua binding rather than an overview dispatcher.
    s.key(125, 1)
    try:
        s.key(30, 1)
        s.key(30, 0)
    finally:
        s.key(125, 0)
    wait_for(lambda: s.status()["live"] and s.status()["keyboard_owned"])
    assert not s.status()["space_selector"]["sidebar_visible"]
    s.ctl("eval", 'local r = hl.plugin.hyprspace.sidebar("on"); assert(r.ok, r.error)')
    wait_for(lambda: s.status()["space_selector"]["sidebar_visible"])
    generation = s.status()["space_selector"]["generation"]
    s.ctl("eval", 'local r = hl.plugin.hyprspace.sidebar("on"); assert(r.ok, r.error)')
    assert s.status()["space_selector"]["generation"] == generation
    s.ctl("eval", 'local r = hl.plugin.hyprspace.sidebar("off"); assert(r.ok, r.error)')
    wait_for(lambda: not s.status()["space_selector"]["sidebar_visible"])
    assert s.status()["live"]
    s.key(29, 1)
    try:
        s.key(57, 1)
        s.key(57, 0)
    finally:
        s.key(29, 0)
    wait_for(lambda: s.status()["space_selector"]["sidebar_visible"])
    assert s.status()["space_selector"]["destination"] == hobby
    s.key(108, 1)
    s.key(108, 0)
    wait_for(lambda: s.status()["space_selector"]["destination"] == original)
    assert state(s)["active"] == hobby, "arrow navigation must only highlight a space"
    s.key(28, 1)
    s.key(28, 0)
    wait_for(lambda: state(s)["active"] == original and all_outputs(s, original)
             and not s.status()["space_selector"]["sidebar_visible"])
    selected = s.status()
    assert selected["live"] and selected["keyboard_owned"]
    s.close()
    s.check("the existing Super+A Lua binding opens the overview with its drawer hidden; Ctrl+Space and arrows/Enter switch spaces inside it without a desktop entry")


def metadata_events(s, wait_for):
    """Native event subscribers can refresh metadata without a polling timer."""
    import select
    import socket
    import time

    path = s.root / "hypr" / s.env["HYPRLAND_INSTANCE_SIGNATURE"] / ".socket2.sock"
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as subscriber:
        subscriber.connect(str(path))
        subscriber.setblocking(False)
        buffered = ""

        def collect(duration):
            nonlocal buffered
            deadline = time.monotonic() + duration
            while (remaining := deadline - time.monotonic()) > 0:
                if not select.select([subscriber], [], [], remaining)[0]:
                    break
                data = subscriber.recv(65536)
                assert data, "native event socket closed"
                buffered += data.decode()
            lines = buffered.split("\n")
            buffered = lines.pop()
            return lines

        def changes(lines):
            return [line for line in lines if line.startswith("custom>>hyprspace:spacechanged,")]

        collect(.2)
        original = state(s)["active"]
        command(s, "rename", id=original, name="Renamed without native workspace mutation")
        observed = collect(.3)
        assert changes(observed) == ["custom>>hyprspace:spacechanged," + original], observed
        assert not any(line.startswith(("workspace>>", "workspacev2>>", "renameworkspace>>")) for line in observed), observed
        assert space(s, original)["name"] == "Renamed without native workspace mutation"
        s.check("a metadata-only space rename publishes one coalesced native custom event without requiring workspace events or polling")

        process = s.spawn(["python3", str(s.client), "hs-space-event-count"])
        wait_for(lambda: "hs-space-event-count" in s.windows())
        assert changes(collect(.3))
        assert sum(ws["windows"] for ws in space(s, original)["workspaces"]) == 1
        process.terminate()
        process.wait(timeout=3)
        wait_for(lambda: not s.windows())
        assert changes(collect(.3))
        assert sum(ws["windows"] for ws in space(s, original)["workspaces"]) == 0
        s.ctl("dispatch", "hyprspace:overview", "on")
        wait_for(lambda: s.status()["live"])
        collect(.2)
        for _ in range(6):
            s.status()
        assert not changes(collect(.25)), "steady rendering and status reads must not publish model-change events"
        s.close()
        s.check("window membership publishes count updates, while steady overview frames and status queries publish no change events")


def run(s, wait_for):
    s.setup("dwindle", 0, 1)
    original = state(s)["active"]
    initial = {title: (w["address"], w["workspace"]["id"], w["at"], w["size"])
               for title, w in s.windows().items()}
    assert {w[1] for w in initial.values()} == {11, 12}
    assert {m["activeWorkspace"]["id"] for m in s.data("monitors")} == {11, 12, 13}
    command(s, "rename", id=original, name="Original desktop")
    assert space(s, original)["name"] == "Original desktop"
    command(s, "rename", id=original, name="Default")
    s.check("a single Default space preserves existing native numeric IDs; Default can be renamed normally")

    # Native scratch IDs are reused after collection. A retained logical
    # record must release that reservation before adopting a different name.
    address = s.windows()["hs-A"]["address"]
    s.ctl("dispatch", "movetoworkspacesilent", "special:hs-space-scratch-old,address:" + address)
    reused_id = wait_for(lambda: s.windows()["hs-A"]["workspace"]["id"]
                         if s.windows()["hs-A"]["workspace"]["name"] == "special:hs-space-scratch-old" else None)
    old_scratch = next(ws["id"] for ws in space(s, original)["workspaces"]
                       if ws["native_id"] == reused_id)
    s.ctl("dispatch", "movetoworkspacesilent", "11,address:" + address)
    wait_for(lambda: not any(ws["id"] == reused_id for ws in s.data("workspaces")))
    s.ctl("dispatch", "movetoworkspacesilent", "special:hs-space-scratch-new,address:" + address)
    wait_for(lambda: s.windows()["hs-A"]["workspace"]["name"] == "special:hs-space-scratch-new")
    assert s.windows()["hs-A"]["workspace"]["id"] == reused_id
    assert owner(s, reused_id) == original
    retained = next(ws for ws in space(s, original)["workspaces"] if ws["id"] == old_scratch)
    adopted = next(ws for ws in space(s, original)["workspaces"] if ws["native_id"] == reused_id)
    assert retained["native_id"] == 0 and adopted["id"] != old_scratch
    assert adopted["name"] == "hs-space-scratch-new"
    s.ctl("dispatch", "hyprspace:overview", "on")
    wait_for(lambda: any(tile["workspace"] == reused_id for view in s.status()["views"] for tile in view["tiles"]))
    assert s.status()["live"]
    s.close()
    s.ctl("dispatch", "movetoworkspacesilent", "11,address:" + address)
    wait_for(lambda: s.windows()["hs-A"]["workspace"]["id"] == 11)
    s.check("reused native scratch IDs retain correct logical ownership and remain visible in the overview after the former scratchpad is collected")

    # Retain a numeric reservation after its empty native workspace is collected,
    # then change the compositor's construction-time display-name rule.
    s.ctl("dispatch", "focusmonitor", s.names[0])
    s.ctl("dispatch", "workspace", "71")
    reserved_default = local(s, original, 71)["id"]
    s.ctl("dispatch", "workspace", "11")
    wait_for(lambda: not any(ws["id"] == 71 for ws in s.data("workspaces")))
    s.ctl("keyword", "workspace", "71,defaultName:Course desk")

    hobby = create(s, wait_for, "Hobby")
    switch(s, wait_for, original)
    s.ctl("dispatch", "focusmonitor", s.names[0])
    s.ctl("dispatch", "workspace", "71")
    wait_for(lambda: s.data("activeworkspace")["id"] == 71)
    rematerialized = local(s, original, 71)
    assert rematerialized["id"] == reserved_default and rematerialized["name"] == "Course desk", rematerialized
    assert owner(s, 71) == original and state(s)["error"] == ""
    s.ctl("dispatch", "workspace", "11")
    switch(s, wait_for, hobby)
    s.check("numeric Default workspaces retain their logical UUID when collected and rebuilt with a new defaultName rule")
    for number in (1, 3, 5, 7):
        window = spawn(s, wait_for, f"hs-space-hobby-{number}", number)
        assert local(s, hobby, number)["native_id"] == window["workspace"]["id"]
    # Native defaultName ranges may give several numeric workspaces the
    # same display name. Managed aliases must remain unique and serializable.
    s.ctl("keyword", "workspace", "r[81-82],defaultName:Shared course")
    for number in (81, 82):
        spawn(s, wait_for, f"hs-space-alias-{number}", number)
    aliases = [local(s, hobby, number) for number in (81, 82)]
    assert {item["name"] for item in aliases} == {"Shared course", "Shared course (2)"}, aliases
    assert len({item["id"] for item in aliases}) == 2
    assert all(item["native_name"].startswith("hyprspace:") for item in aliases)
    alias_file = s.root / "state/hyprspace/spaces.json"

    def saved_aliases():
        if not alias_file.exists():
            return False
        saved = json.loads(alias_file.read_text())
        by_id = {item["id"]: item for item in saved["workspaces"]}
        return all(item["id"] in by_id and by_id[item["id"]]["name"] == item["name"] for item in aliases)

    wait_for(saved_aliases)
    assert state(s)["error"] == "", state(s)["error"]
    s.check("repeated defaultName rules retain distinct managed workspace identities, unique logical labels and valid saved state")

    hobby_one = local(s, hobby, 1)["native_id"]
    hobby_layout = {title: (w["workspace"]["id"], w["at"], w["size"])
                    for title, w in s.windows().items() if title.startswith("hs-space-hobby-")}
    work = create(s, wait_for, "Work")
    work_window = spawn(s, wait_for, "hs-space-work", 1)
    work_one = local(s, work, 1)["native_id"]
    assert hobby_one != work_one
    assert owner(s, work_window["workspace"]["id"]) == work
    assert {title: (s.windows()[title]["address"], s.windows()[title]["workspace"]["id"])
            for title in initial} == {title: value[:2] for title, value in initial.items()}
    rejected(s, "create", name=" Work ")
    command(s, "rename", id=hobby, name="ATDL")
    assert space(s, hobby)["name"] == "ATDL"
    switch(s, wait_for, hobby)
    assert {title: (s.windows()[title]["workspace"]["id"], s.windows()[title]["at"], s.windows()[title]["size"])
            for title in hobby_layout} == hobby_layout
    s.check("spaces reuse logical 1/3/5/7 with independent native workspaces, preserve applications and switch all outputs together")

    # Ordinary bindings continue addressing local numbers and the compositor's previous workspace.
    s.ctl("dispatch", "focusmonitor", s.names[0])
    s.ctl("dispatch", "workspace", "50")
    fifty = local(s, hobby, 50)["native_id"]
    s.ctl("dispatch", "workspace", "+1")
    assert s.data("activeworkspace")["id"] == local(s, hobby, 51)["native_id"]
    s.ctl("dispatch", "workspace", "previous")
    assert s.data("activeworkspace")["id"] == fifty
    s.ctl("dispatch", "workspace", "e+1")
    assert owner(s, s.data("activeworkspace")["id"]) == hobby
    s.ctl("keyword", "workspace", f"41,monitor:{s.names[1]},persistent:true")
    wait_for(lambda: all(local(s, identity, 41) is not None for identity in (original, hobby, work)))
    s.ctl("dispatch", "workspace", "41")
    target = local(s, hobby, 41)
    assert s.data("activeworkspace")["id"] == target["native_id"]
    assert target["monitor"] == s.names[1]
    s.ctl("dispatch", "renameworkspace", "41 lab")
    assert local(s, hobby, 41)["name"] == "lab"
    s.ctl("dispatch", "workspace", "name:lab")
    assert s.data("activeworkspace")["id"] == target["native_id"]
    s.ctl("dispatch", "workspace", "lab")
    assert s.data("activeworkspace")["id"] == target["native_id"]
    s.ctl("keyword", "workspace", f"53,monitor:{s.names[1]},persistent:true")
    s.ctl("dispatch", "focusmonitor", s.names[0])
    s.ctl("dispatch", "workspace", "52")
    s.ctl("dispatch", "workspace", "r+1")
    assert s.data("activeworkspace")["id"] == local(s, hobby, 54)["native_id"]
    s.ctl("dispatch", "workspace", "r~1")
    assert s.data("activeworkspace")["id"] == local(s, hobby, 1)["native_id"]
    # Empty workspaces can be collected as soon as they become inactive;
    # m/e selectors deliberately enumerate only live native workspaces.
    live = {ws["id"] for ws in s.data("workspaces")}
    expected = max((ws for ws in space(s, hobby)["workspaces"]
                    if not ws["special"] and ws["native_id"] in live and ws["monitor"] == s.names[0]),
                   key=lambda ws: ws["number"])["native_id"]
    s.ctl("dispatch", "workspace", "m~999999")
    assert s.data("activeworkspace")["id"] == expected
    live = {ws["id"] for ws in s.data("workspaces")}
    expected = max((ws for ws in space(s, hobby)["workspaces"]
                    if not ws["special"] and ws["native_id"] in live), key=lambda ws: ws["number"])["native_id"]
    s.ctl("dispatch", "workspace", "e~999999")
    assert s.data("activeworkspace")["id"] == expected
    # Exercise combined logical identity + native dynamic property matching.
    s.ctl("keyword", "workspace", "r[40-42] w[0],gapsin:17")
    s.ctl("keyword", "windowrule", "match:title ^hs-space-rule$, workspace 43 silent")
    s.spawn(["python3", str(s.client), "hs-space-rule"])
    ruled = wait_for(lambda: s.windows().get("hs-space-rule"))
    wait_for(lambda: local(s, hobby, 43) is not None)
    assert ruled["workspace"]["id"] == local(s, hobby, 43)["native_id"]
    s.check("numeric, relative, previous, named destinations, persistent monitor bindings and application workspace rules resolve inside the active space")

    # Pinning spans workspaces within its owner space, and never other spaces.
    pinned = s.windows()["hs-space-hobby-7"]["address"]
    s.ctl("dispatch", "focuswindow", "address:" + pinned)
    s.ctl("dispatch", "setfloating", "address:" + pinned)
    s.ctl("dispatch", "pin", "address:" + pinned)
    wait_for(lambda: s.windows()["hs-space-hobby-7"]["pinned"])
    s.ctl("dispatch", "workspace", "52")
    assert s.windows()["hs-space-hobby-7"]["pinned"]
    scratch = s.windows()["hs-space-hobby-5"]["address"]
    s.ctl("dispatch", "movetoworkspacesilent", "special:notes,address:" + scratch)
    s.ctl("dispatch", "togglespecialworkspace", "notes")
    scratch_id = s.windows()["hs-space-hobby-5"]["workspace"]["id"]
    assert owner(s, scratch_id) == hobby
    switch(s, wait_for, work)
    assert not s.windows()["hs-space-hobby-7"]["pinned"]
    assert all(m["specialWorkspace"]["id"] != scratch_id for m in s.data("monitors"))
    work_scratch = spawn(s, wait_for, "hs-space-work-notes", 7)
    s.ctl("dispatch", "movetoworkspacesilent", "special:notes,address:" + work_scratch["address"])
    assert s.windows()["hs-space-work-notes"]["workspace"]["id"] != scratch_id
    switch(s, wait_for, hobby)
    wait_for(lambda: s.windows()["hs-space-hobby-7"]["pinned"])
    wait_for(lambda: any(m["specialWorkspace"]["id"] == scratch_id for m in s.data("monitors")))
    s.ctl("dispatch", "togglespecialworkspace", "notes")
    s.check("pin intent and identically named special workspaces remain confined to their owning space and restore on return")

    # Give each physical output a different populated normal workspace. Checking
    # only ownership would miss one output stealing another's remembered slot.
    for index, monitor in enumerate(s.names):
        s.ctl("dispatch", "focusmonitor", monitor)
        spawn(s, wait_for, f"hs-space-hotplug-{index}", 121 + index * 2)
    expected_selections = monitor_selections(s, hobby)
    assert len(expected_selections) == 3 and len(set(expected_selections.values())) == 3
    # A detached output's workspaces remain logical members and restore on reload.
    s.ctl("keyword", "monitor", f"{s.names[2]},disable")
    wait_for(lambda: len(s.data("monitors")) == 2)
    switch(s, wait_for, work)
    switch(s, wait_for, hobby)
    assert all_outputs(s, hobby)
    wait_for(lambda: monitor_selections(s, hobby) == {
        monitor: identity for monitor, identity in expected_selections.items() if monitor != s.names[2]})
    before_reload = {item["id"]: item["name"] for item in state(s)["spaces"]}
    s.ctl("reload")
    wait_for(lambda: len(s.data("monitors")) == 3)
    wait_for(lambda: all_outputs(s, hobby))
    wait_for(lambda: monitor_selections(s, hobby) == expected_selections)
    assert {item["id"]: item["name"] for item in state(s)["spaces"]} == before_reload
    path = s.root / "state/hyprspace/spaces.json"

    def saved():
        if not path.exists():
            return False
        value = json.loads(path.read_text())
        selected = next((item["selections"] for item in value.get("spaces", []) if item["id"] == hobby), {})
        return value if value.get("active") == hobby and all(selected.get(monitor) == identity for monitor, identity in expected_selections.items()) else False

    persisted = wait_for(saved)
    assert {item["id"]: item["name"] for item in persisted["spaces"]} == before_reload
    (s.root / "spaces-hotplug-selections.json").write_text(json.dumps({"before": expected_selections, "after": monitor_selections(s, hobby)}, indent=2))
    s.check("spaces retain exact per-output workspace UUIDs across output removal/reconnection, switches and config reload")
    fresh_compositor(s, wait_for, persisted, expected_selections)

    # Both launcher captures and plain desktop exec retain their origin after
    # a space switch, without revealing the late window or stealing input.
    from launch_regressions import delayed

    s.ctl("dispatch", "hyprspace:overview", "on")
    wait_for(lambda: s.status()["live"])
    s.move(s.preview_point("hs-space-hobby-1"))
    launch_workspace = wait_for(lambda: s.status().get("target"))["workspace"]
    token = s.request("capture")
    assert token
    gate = delayed(s, wait_for, token, "hs-space-late-overview")
    s.close()
    switch(s, wait_for, work)
    s.ctl("dispatch", "focuswindow", "address:" + work_window["address"])
    focus_before = s.data("activewindow")["address"]
    outputs_before = {m["name"]: m["activeWorkspace"]["id"] for m in s.data("monitors")}
    gate.touch()
    late = wait_for(lambda: s.windows().get("hs-space-late-overview"))
    wait_for(lambda: s.windows()["hs-space-late-overview"]["workspace"]["id"] == launch_workspace)
    assert owner(s, s.windows()["hs-space-late-overview"]["workspace"]["id"]) == hobby
    assert state(s)["active"] == work and s.data("activewindow")["address"] == focus_before
    assert {m["name"]: m["activeWorkspace"]["id"] for m in s.data("monitors")} == outputs_before

    switch(s, wait_for, hobby)
    s.ctl("dispatch", "focuswindow", "address:" + s.windows()["hs-space-hobby-1"]["address"])
    launch_workspace = s.windows()["hs-space-hobby-1"]["workspace"]["id"]
    gate = s.root / "hs-space-late-desktop.gate"
    ready = s.root / "hs-space-late-desktop.gate.ready"
    program = (
        "import os,sys,time\nfrom pathlib import Path\n"
        "gate=Path(sys.argv[1]); Path(str(gate)+'.ready').touch()\n"
        "while not gate.exists(): time.sleep(.01)\n"
        "os.execv(sys.executable,[sys.executable,*sys.argv[2:]])\n"
    )
    shell = " ".join(shlex.quote(value) for value in
                     ("python3", "-c", program, str(gate), str(s.client), "hs-space-late-desktop"))
    s.ctl("dispatch", "exec", shell)
    wait_for(ready.exists)
    switch(s, wait_for, work)
    s.ctl("dispatch", "focuswindow", "address:" + work_window["address"])
    focus_before = s.data("activewindow")["address"]
    outputs_before = {m["name"]: m["activeWorkspace"]["id"] for m in s.data("monitors")}
    gate.touch()
    wait_for(lambda: s.windows().get("hs-space-late-desktop"))
    wait_for(lambda: s.windows()["hs-space-late-desktop"]["workspace"]["id"] == launch_workspace)
    assert state(s)["active"] == work and s.data("activewindow")["address"] == focus_before
    assert {m["name"]: m["activeWorkspace"]["id"] for m in s.data("monitors")} == outputs_before
    s.check("delayed overview launches and ordinary desktop exec retain their source space without switching outputs or stealing focus")

    # Prove a real dlclose/dlopen cycle, including a pin whose owner is hidden.
    s.close()
    assert state(s)["active"] == work and not s.windows()["hs-space-hobby-7"]["pinned"]
    before_plugin_reload = {item["id"]: item["name"] for item in state(s)["spaces"]}
    windows_before_reload = {title: (w["address"], w["workspace"]["id"])
                             for title, w in s.windows().items()}
    maps = Path(f"/proc/{s.compositor_pid}/maps")
    fixture = s.artifact("test-overview.so")
    fixture_loaded = str(fixture) in maps.read_text()
    if fixture_loaded:
        s.ctl("plugin", "unload", str(fixture))
        wait_for(lambda: str(fixture) not in maps.read_text())
    s.ctl("plugin", "unload", str(s.plugin))
    wait_for(lambda: str(s.plugin) not in maps.read_text())
    assert s.data("monitors") and s.compositor.poll() is None
    saved_model = json.loads(path.read_text())
    assert any(key.split("/")[-2] == pinned.removeprefix("0x") and desired
               for key, desired in saved_model["pin_intents"].items())
    s.ctl("plugin", "load", str(s.plugin))
    wait_for(lambda: str(s.plugin) in maps.read_text())
    s.ctl("reload")
    wait_for(lambda: state(s)["active"] == work and all_outputs(s, work))
    assert {item["id"]: item["name"] for item in state(s)["spaces"]} == before_plugin_reload
    assert {title: (w["address"], w["workspace"]["id"])
            for title, w in s.windows().items()} == windows_before_reload
    assert not s.windows()["hs-space-hobby-7"]["pinned"]
    switch(s, wait_for, hobby)
    wait_for(lambda: s.windows()["hs-space-hobby-7"]["pinned"])
    switch(s, wait_for, work)
    assert not s.windows()["hs-space-hobby-7"]["pinned"]
    if fixture_loaded:
        s.ctl("plugin", "load", str(fixture))
    s.check("actual plugin unmapping/reload preserves space identities, native window placement and inactive-space desired pin state")

    temporary = create(s, wait_for, "Transfer source")
    single = spawn(s, wait_for, "hs-space-single-transfer", 9)
    active_before = state(s)["active"]
    move(s, target="window", space_id=hobby, window=single["address"])
    wait_for(lambda: owner(s, s.windows()["hs-space-single-transfer"]["workspace"]["id"]) == hobby)
    assert state(s)["active"] == active_before and all_outputs(s, temporary)
    whole = spawn(s, wait_for, "hs-space-workspace-transfer", 10)
    native_id = whole["workspace"]["id"]
    workspace_id = local(s, temporary, 10)["id"]
    move(s, target="workspace", space_id=hobby, workspace=native_id)
    wait_for(lambda: owner(s, native_id) == hobby)
    assert s.windows()["hs-space-workspace-transfer"]["workspace"]["id"] == native_id
    assert next(ws for ws in space(s, hobby)["workspaces"] if ws["id"] == workspace_id)["native_id"] == native_id
    assert all_outputs(s, temporary)
    populated = spawn(s, wait_for, "hs-space-delete-transfer", 11)
    deletion_id = populated["workspace"]["id"]
    addresses = {title: w["address"] for title, w in s.windows().items()}
    command(s, "delete", id=temporary, target=work)
    wait_for(lambda: state(s)["active"] == work and all_outputs(s, work))
    assert owner(s, deletion_id) == work
    assert s.windows()["hs-space-delete-transfer"]["workspace"]["id"] == deletion_id
    assert {title: w["address"] for title, w in s.windows().items()} == addresses
    s.check("window moves preserve the active space; whole workspace moves and populated-space deletion retain native layouts and live applications")

    command(s, "delete", id=original, target=work)
    assert not any(item["id"] == original for item in state(s)["spaces"])
    for title, value in initial.items():
        assert s.windows()[title]["address"] == value[0]
        assert s.windows()[title]["workspace"]["id"] == value[1]
        assert owner(s, value[1]) == work
    command(s, "delete", id=hobby, target=work)
    assert len(state(s)["spaces"]) == 1
    rejected(s, "delete", id=work, target=work)
    assert len(state(s)["spaces"]) == 1 and all_outputs(s, work)
    assert {title: w["address"] for title, w in s.windows().items()} == addresses
    s.check("Default is deletable, all source workspaces transfer without closing windows, and the final space is protected")
