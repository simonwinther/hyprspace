"""Searchable help: input leases, configured controls, geometry and native handoff."""

import json
import subprocess
import time

from protocol import reply
from resize import area, inside
from zoom import center, inspection, opened, same_geometry, tile, wheel


def help_state(s):
    return s.status()["help"]


def tap(s, code):
    s.key(code, 1)
    s.key(code, 0)


def leader(s, code):
    tap(s, 57)
    assert s.status()["input"]["prefix"] == "space", s.status()["input"]
    tap(s, code)


def question(s):
    s.key(42, 1)
    tap(s, 53)
    s.key(42, 0)


def search(s, text):
    s.run("wtype", "/" + text)


def run(s, wait_for):
    panel = None
    dk = None
    try:
        s.setup("dwindle", 0, 1)
        opened(s, wait_for)
        target = s.status()["target"]
        native = s.geometry()
        focus = s.data("activewindow")["address"]
        s.key(42, 1)
        s.key(53, 1)
        wait_for(lambda: help_state(s)["active"])
        s.key(53, 1)
        time.sleep(.3)
        assert help_state(s)["active"] and not help_state(s)["search_focused"]
        s.key(53, 0)
        s.key(42, 0)
        initial = help_state(s)
        assert initial["monitor"] == s.names[0] and initial["count"] >= 25
        assert next(e for e in initial["entries"] if e["id"] == "overview")["keys"] == "Super + A"
        s.run("grim", "-s", "1", "-o", s.names[0], str(s.root / "help-browse.png"))
        s.check("? opens help on the selected output, shows configured native shortcuts and acts once per physical press")

        tap(s, 36)
        assert help_state(s)["selected_index"] == 1
        tap(s, 37)
        assert help_state(s)["selected_index"] == 0
        tap(s, 38)
        assert help_state(s)["topic"] == "Navigation"
        tap(s, 35)
        assert help_state(s)["topic"] == "All help"
        tap(s, 107)
        assert help_state(s)["selected_index"] == help_state(s)["count"] - 1 and help_state(s)["scroll"] > 0
        tap(s, 102)
        s.scroll(45, 3)
        assert help_state(s)["selected_index"] == 3
        box = help_state(s)["results_box"]
        s.move((box["x"] + 30, box["y"] + help_state(s)["row_height"] / 2))
        s.button(1); s.button(0)
        assert s.status()["target"] == target and s.geometry() == native
        assert s.data("activewindow")["address"] == focus
        s.check("vim keys, topics, Home/End, wheel and row clicks browse help without selecting or opening anything behind it")

        search(s, "keybinds")
        wait_for(lambda: help_state(s)["query"] == "keybinds")
        assert help_state(s)["selected_id"] == "bindings" and help_state(s)["count"] > 10
        s.run("grim", "-s", "1", "-o", s.names[0], str(s.root / "help-keybinds.png"))
        tap(s, 28)
        assert help_state(s)["active"] and not help_state(s)["search_focused"]
        tap(s, 36)
        assert help_state(s)["selected_index"] == 1
        assert s.status()["target"] == target and s.status()["live"]
        search(s, "resize")
        assert help_state(s)["query"] == "resize" and help_state(s)["selected_id"] == "resize-window"
        assert help_state(s)["count"] == 1
        s.check("/keybinds finds configuration and controls; Enter browses results and a new / search replaces the query across topics")

        s.key(29, 1); tap(s, 30); s.key(29, 0)
        s.run("wtype", "æøå")
        assert help_state(s)["query"] == "æøå" and help_state(s)["query_cursor"] == 6
        tap(s, 14)
        assert help_state(s)["query"] == "æø"
        s.key(42, 1); tap(s, 105); s.key(42, 0)
        s.run("wtype", "X")
        assert help_state(s)["query"] == "æX"
        question(s)
        assert help_state(s)["active"] and help_state(s)["query"] == "æX?"
        assert help_state(s)["count"] == 0 and help_state(s)["selected_id"] == ""
        s.run("grim", "-s", "1", "-o", s.names[0], str(s.root / "help-no-results.png"))
        s.key(29, 1); tap(s, 22); s.key(29, 0)
        assert help_state(s)["query"] == "" and help_state(s)["search_focused"]
        s.key(1, 1); s.key(1, 1)
        assert help_state(s)["active"] and not help_state(s)["search_focused"]
        s.key(1, 0)
        tap(s, 1)
        assert not help_state(s)["active"] and s.status()["live"] and s.status()["target"] == target
        s.check("Unicode editing, Shift selection, literal ? in search, empty results and two-level Escape work without leaking input")

        s.key(42, 1); s.key(53, 1)
        wait_for(lambda: help_state(s)["active"])
        tap(s, 1)
        s.ctl("dispatch", "hyprspace:help", "on")
        generation = help_state(s)["generation"]
        s.key(53, 1)
        assert help_state(s)["active"] and help_state(s)["generation"] == generation
        s.key(53, 0); s.key(42, 0)
        s.key(36, 1)
        s.ctl("dispatch", "hyprspace:help", "off")
        s.ctl("dispatch", "hyprspace:help", "on")
        before = help_state(s)["selected_index"]
        s.key(36, 1)
        assert help_state(s)["selected_index"] == before
        s.key(36, 0)
        s.check("held help and navigation keys stay captured across close/reopen and cannot act in a newer help generation")

        for option, value in (("zoom_key", "x"), ("follow_key", "c"), ("window_view_key", "v"), ("empty_workspace_key", "b"), ("workspace_drag_modifiers", "SUPER SHIFT")):
            s.ctl("keyword", "plugin:hyprspace:overview:" + option, value)
        wait_for(lambda: next(e for e in help_state(s)["entries"] if e["id"] == "move-workspace")["keys"] == "Super + Shift + drag")
        entries = {e["id"]: e for e in help_state(s)["entries"]}
        assert entries["zoom"]["keys"] == "Hold X" and entries["follow"]["keys"].endswith(" + C")
        assert entries["window-board"]["keys"] == "Space → v"
        assert entries["groups"]["keys"] == "Space → g" and entries["recent"]["keys"] == "Space → r"
        assert entries["empty"]["keys"].startswith("Space → n")
        assert entries["move-workspace"]["keys"] == "Super + Shift + drag"
        s.ctl("keyword", "plugin:hyprspace:overview:help_key", "F1")
        assert next(e for e in help_state(s)["entries"] if e["id"] == "help")["keys"] == "? / Space → ?"
        search(s, "follow")
        tap(s, 59)  # An optional Normal-mode alias cannot discard Insert text.
        assert help_state(s)["active"] and help_state(s)["query"] == "follow" and help_state(s)["search_focused"]
        tap(s, 28)
        tap(s, 59)
        assert not help_state(s)["active"]
        question(s)  # The shared ? command remains available with another alias.
        assert help_state(s)["active"]
        tap(s, 59)
        assert not help_state(s)["active"]
        tap(s, 59)
        assert help_state(s)["active"]
        s.ctl("keyword", "plugin:hyprspace:overview:help_key", "")
        tap(s, 59)
        assert help_state(s)["active"]
        tap(s, 1)
        question(s)
        assert help_state(s)["active"]
        tap(s, 1)
        s.ctl("keyword", "plugin:hyprspace:overview:help_key", "question")
        for option, value in (("zoom_key", "z"), ("follow_key", "f"), ("window_view_key", "g"), ("empty_workspace_key", "n"), ("workspace_drag_modifiers", "SUPER ALT")):
            s.ctl("keyword", "plugin:hyprspace:overview:" + option, value)
        s.check("help reflects inspection and drag settings, retains fixed leader commands despite deprecated remaps, and supports an optional Normal-mode help alias")

        question(s)
        search(s, "follow")
        s.key(33, 1)
        panel = s.spawn(["python3", str(s.artifact("layer.py")), str(s.root / "help-foreground"), "help-foreground"])
        wait_for(lambda: not s.status()["keyboard_owned"])
        before = help_state(s)["query"]
        s.run("wtype", "native")
        wait_for(lambda: (s.root / "help-foreground").exists() and (s.root / "help-foreground").read_text() == "native")
        panel.terminate(); panel.wait(timeout=3); panel = None
        wait_for(lambda: s.status()["keyboard_owned"])
        s.key(33, 1)
        assert help_state(s)["query"] == before
        s.key(33, 0)
        assert help_state(s)["active"]
        tap(s, 1); tap(s, 1)
        s.check("foreground keyboard ownership preserves help and its query while invalidating held text across handoff")

        leader(s, 47)  # Space v
        wait_for(lambda: s.status()["window_board"]["active"])
        selected = s.status()["window_board"]["selected_window"]
        question(s)
        search(s, "resize")
        tap(s, 1); tap(s, 1)
        assert s.status()["window_board"]["active"] and s.status()["window_board"]["selected_window"] == selected
        s.run("wtype", "/?")
        assert s.status()["window_board"]["query"] == "?" and not help_state(s)["active"]
        s.ctl("keyword", "plugin:hyprspace:overview:help_key", "F1")
        tap(s, 59)
        assert not help_state(s)["active"] and s.status()["window_board"]["search_focused"]
        assert s.status()["window_board"]["query"] == "?"
        tap(s, 28)
        tap(s, 59)
        assert help_state(s)["active"] and s.status()["window_board"]["query"] == "?"
        tap(s, 59)
        assert not help_state(s)["active"] and s.status()["window_board"]["query"] == "?"
        s.ctl("keyword", "plugin:hyprspace:overview:help_key", "question")
        leader(s, 17)  # Space w returns to the workspace canvas.
        assert not s.status()["window_board"]["active"]
        s.check("help returns to the same board selection and retained Normal query; ? stays literal in Insert and optional aliases do not interrupt editing")

        opened(s, wait_for)
        s.key(44, 1); inspection(s, wait_for, 1)
        s.move(center(tile(s, 11)))
        tap(s, 33)
        assert s.status()["zoom"]["cursor_follow_enabled"]
        wheel(s, -32); inspection(s, wait_for, 4)
        framed = tile(s, 11)
        question(s)
        s.motion(100, 30)
        tap(s, 13); tap(s, 33)
        time.sleep(.2)
        assert s.status()["zoom"]["extra_factor"] == 4 and s.status()["zoom"]["cursor_follow_enabled"]
        assert not s.status()["zoom"]["cursor_follow_settling"]
        assert same_geometry({0: framed}, {0: tile(s, 11)})
        s.key(44, 0)
        wait_for(lambda: not s.status()["zoom"]["held"])
        assert help_state(s)["active"]
        tap(s, 1)
        s.key(44, 1); inspection(s, wait_for, 1); tap(s, 33); s.key(44, 0)
        assert not s.status()["zoom"]["cursor_follow_enabled"]
        s.check("help pauses magnified follow and inspection controls without changing the preference; releasing the zoom key remains valid")

        s.close()
        for output in s.names:
            mon = next(m for m in s.data("monitors") if m["name"] == output)
            s.ctl("dispatch", "focusmonitor", output)
            s.move(center(area(mon)))
            s.ctl("dispatch", "hyprspace:overview", "on")
            wait_for(lambda: s.status()["live"])
            question(s)
            wait_for(lambda: help_state(s)["active"])
            h = help_state(s)
            assert h["monitor"] == output, (output, h["monitor"])
            assert inside(h["panel"], area(mon)) and inside(h["search_box"], h["panel"])
            s.run("grim", "-s", "1", "-o", output, str(s.root / ("help-" + output + ".png")))
            tap(s, 1)
            s.close()
        s.check("help remains inside usable bounds on offset, rotated and fractional-scale outputs with responsive layout")

        opened(s, wait_for)
        dk_env = s.env.copy(); dk_env["HS_KEYBOARD_LAYOUT"] = "dk"
        dk = subprocess.Popen([str(s.artifact("test-pointer"))], env=dk_env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=(s.root / "help-danish.log").open("w"), text=True)
        s.processes.append(dk)
        assert reply(dk) == "ready"
        def dk_key(code, pressed):
            dk.stdin.write(f"key {code} {pressed}\n"); dk.stdin.flush(); assert reply(dk) == "ok"
        for resolve in ("true", "false"):
            s.ctl("keyword", "input:resolve_binds_by_sym", resolve)
            dk_key(42, 1); dk_key(12, 1); dk_key(12, 0); dk_key(42, 0)
            wait_for(lambda: help_state(s)["active"])
            dk_key(42, 1); dk_key(8, 1); dk_key(8, 0); dk_key(42, 0)
            assert help_state(s)["search_focused"] and help_state(s)["query"] == ""
            s.run("wtype", "keybinds")
            assert help_state(s)["selected_id"] == "bindings"
            tap(s, 1); tap(s, 1)
        s.check("Danish ? and / work through their resolved characters with either native binding-resolution setting")
        dk.terminate(); dk.wait(timeout=3); dk = None

        question(s)
        search(s, "follow")
        s.ctl("reload")
        wait_for(lambda: help_state(s)["active"] and help_state(s)["query"] == "follow")
        s.close()
        assert not help_state(s)["active"]
        wait_for(lambda: s.status()["resources"]["captures"]["bytes"] == 0 and s.status()["resources"]["textures"]["entries"] == 0)
        s.check("configuration reload preserves help; overview dismissal releases its input, caret and preview captures")

        opened(s, wait_for)
        question(s)
        search(s, "keybinds")
        s.key(36, 1)
        s.ctl("plugin", "unload", str(s.plugin))
        s.key(36, 0)
        time.sleep(.6)
        s.ctl("plugin", "load", str(s.plugin))
        wait_for(lambda: not s.status()["help"]["active"])
        assert not s.status()["keyboard_owned"] and not s.status()["cursor_owned"]
        assert s.status()["resources"]["captures"]["bytes"] == 0
        opened(s, wait_for)
        question(s)
        assert help_state(s)["active"]
        tap(s, 1)
        s.close()
        s.check("unloading active search releases held keys and its caret timer; reloading restores normal help activation")
    except BaseException:
        try:
            (s.root / "help-failure.json").write_text(json.dumps(s.status(), indent=2))
            host = help_state(s).get("monitor", s.names[0])
            s.run("grim", "-s", "1", "-o", host, str(s.root / "help-failure.png"))
        except Exception:
            pass
        raise
    finally:
        for process in (panel, dk):
            if process is not None and process.poll() is None:
                process.terminate(); process.wait(timeout=3)
        for code in (1, 42, 53, 29, 33, 36, 44, 59):
            s.key(code, 0)
        s.ctl("keyword", "input:resolve_binds_by_sym", "true")
        for option, value in (("help_key", "question"), ("zoom_key", "z"), ("follow_key", "f"), ("window_view_key", "g"), ("empty_workspace_key", "n"), ("workspace_drag_modifiers", "SUPER ALT")):
            s.ctl("keyword", "plugin:hyprspace:overview:" + option, value)
        s.close()
