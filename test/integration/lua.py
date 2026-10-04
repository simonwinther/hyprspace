"""Lua dispatcher routing across configuration-state replacement."""

import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import time

from artifacts import Snapshot, prune_snapshots
from background import BackgroundDisplay


def suite_type(base, wait_for):
    class LuaSuite(base):
        def config_text(self, plugin=True):
            command = shlex.join(["python3", str(self.client), "hs-lua-tool"])
            text = f'''local marker = assert(io.open({json.dumps(str(self.root / "lua-reloads.log"))}, "a"))
marker:write("loaded\\n")
marker:close()
hl.monitor({{ output = "", mode = "960x600@60", position = "auto", scale = 1 }})
hl.config({{
    misc = {{ disable_hyprland_logo = true, disable_splash_rendering = true }},
    animations = {{ enabled = false }},
    input = {{ follow_mouse = 0, resolve_binds_by_sym = true }},
    debug = {{ disable_logs = false }},
}})
hl.device({{ name = "wl_keyboard", enabled = false }})
hl.device({{ name = "wl_pointer", enabled = false }})
hl.bind("SUPER + 8", hl.dsp.focus({{ workspace = "8" }}))
hl.bind("SUPER + B", hl.dsp.exec_cmd({json.dumps(command)}))
hl.monitor({{ output = "WAYLAND-1", mode = "960x600@60", position = "0x0", scale = 1 }})
hl.monitor({{ output = "WAYLAND-2", mode = "960x600@60", position = "-1000x-200", scale = 1.25, transform = 1 }})
hl.monitor({{ output = "WAYLAND-3", mode = "960x600@60", position = "2200x100", scale = 1.5 }})
'''
            if plugin:
                text += f"hl.plugin.load({json.dumps(str(self.plugin))})\n"
            return text

        def reload_count(self):
            path = self.root / "lua-reloads.log"
            return len(path.read_text().splitlines()) if path.exists() else 0

        def start(self):
            assert not self.visible and not self.packaged_plugin
            self.root.chmod(0o700)
            if self.snapshot is None:
                prune_snapshots(self.root.parent)
                self.snapshot = Snapshot.create(self.root, self.build_dir / "integration.json")
            self.plugin = self.artifact("hyprspace.so")
            self.client = self.artifact("client.py")
            (self.root / "generation.json").write_text(json.dumps(self.snapshot.record, indent=2))
            self.background = BackgroundDisplay(self.root, self.env, self.artifact("test-headless"))
            self.env["WAYLAND_DISPLAY"] = self.background.start()
            self.config = self.root / "hyprland.lua"
            self.config.write_text(self.config_text(plugin=False))
            boot = 'import os,sys; from pathlib import Path; Path(sys.argv[1]).write_text(os.environ["DBUS_SESSION_BUS_ADDRESS"]); os.execvp("Hyprland", ["Hyprland", "--config", sys.argv[2]])'
            log = (self.root / "compositor.log").open("w")
            self.compositor = subprocess.Popen(
                ["dbus-run-session", "--", "python3", "-c", boot, str(self.root / "dbus"), str(self.config)],
                env=self.env, stdout=log, stderr=log, start_new_session=True,
            )

            def ready():
                if self.compositor.poll() is not None:
                    raise RuntimeError(f"Test compositor exited; see {self.root / 'compositor.log'}")
                return list((self.root / "hypr").glob("*/.socket.sock"))

            wait_for(ready, timeout=15)
            instance = next((self.root / "hypr").glob("*/.socket.sock")).parent.name
            display = next(p for p in self.root.glob("wayland-*") if not p.name.endswith(".lock"))
            self.env.update(
                HYPRLAND_INSTANCE_SIGNATURE=instance, WAYLAND_DISPLAY=display.name,
                DBUS_SESSION_BUS_ADDRESS=(self.root / "dbus").read_text(),
            )
            self.connected = True
            self.record_compositor()
            keys = ("XDG_RUNTIME_DIR", "WAYLAND_DISPLAY", "HYPRLAND_INSTANCE_SIGNATURE", "DBUS_SESSION_BUS_ADDRESS")
            (self.root / "env.json").write_text(json.dumps({key: self.env[key] for key in keys}))
            (self.root / "session.json").write_text(json.dumps({"mode": "headless", "outputs": self.names}))
            self.await_outputs(1)
            self.ctl("output", "create", "wayland")
            self.await_outputs(2)
            self.ctl("output", "create", "wayland")
            self.await_outputs(3)
            wait_for(lambda: {m["name"]: m["scale"] for m in self.data("monitors")} == dict(zip(self.names, (1, 1.25, 1.5))))
            count = self.reload_count()
            self.config.write_text(self.config_text())
            self.ctl("plugin", "load", str(self.plugin))
            wait_for(lambda: self.reload_count() > count)
            wait_for(lambda: not self.status()["views"])
            assert self.ctl("configerrors") == ""
            self.check("Lua plugin startup completes its queued configuration reload")

        def close(self):
            self.ctl("eval", "hl.plugin.hyprspace.close()")
            wait_for(lambda: not self.status()["live"] and not self.status()["views"])

    return LuaSuite


def lifecycle(s, wait_for):
    evidence = {"generation": s.snapshot.record["generation"], "cases": []}
    destination = s.root / "lua-routing.json"

    def save():
        destination.write_text(json.dumps(evidence, indent=2))

    def state(plugin=True):
        result = {name: s.data(name) for name in ("activeworkspace", "monitors", "workspaces", "cursorpos", "binds")}
        if plugin:
            result["overview"] = s.status()
        return result

    def clear_windows():
        for pid in {window["pid"] for window in s.windows().values()}:
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        for process in s.processes[1:]:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
        s.processes = s.processes[:1]
        wait_for(lambda: not s.windows())

    def prepare(plugin):
        if plugin:
            s.close()
        clear_windows()
        s.ctl("eval", "hl.config({ animations = { enabled = false }, input = { follow_mouse = 0 } })")
        for index, title in ((0, "hs-A"), (1, "hs-B")):
            s.ctl("eval", f'hl.dispatch(hl.dsp.focus({{ monitor = "{s.names[index]}" }}))')
            s.ctl("eval", f'hl.dispatch(hl.dsp.focus({{ workspace = "{11 + index}" }}))')
            s.spawn(["python3", str(s.client), title])
            wait_for(lambda: title in s.windows())
        s.ctl("eval", f'hl.dispatch(hl.dsp.focus({{ monitor = "{s.names[0]}" }}))')
        s.ctl("eval", 'hl.dispatch(hl.dsp.focus({ workspace = "11" }))')
        if plugin:
            s.ctl("eval", "hl.config({ plugin = { hyprspace = { follow_mouse = true } }, cursor = { no_warps = false } })")
            s.ctl("eval", 'hl.plugin.hyprspace.overview("on")')
            wait_for(lambda: any(tile["window"] == s.windows()["hs-B"]["address"] for view in s.status()["views"] for tile in view["tiles"]))
            time.sleep(0.15)
            point = s.preview_point("hs-B", 0.15, 0.75)
            cursor = s.data("cursorpos")
            # Native movecursor can change monitor focus and mask this defect.
            s.motion(point[0] - cursor["x"], point[1] - cursor["y"])
            wait_for(lambda: s.status()["target"]["workspace"] == 12)
        before = state(plugin)
        assert before["activeworkspace"]["id"] == 11, before
        if plugin:
            assert before["overview"]["target"]["workspace"] == 12, before
        assert not any(workspace["id"] == 8 for workspace in before["workspaces"]), before
        return before

    def route(phase, command, plugin=True):
        case = {"phase": phase, "command": command, "plugin": plugin, "before": prepare(plugin)}
        evidence["cases"].append(case)
        save()
        s.run("wtype", "-M", "logo", "-k", "8" if command == "workspace" else "b", "-m", "logo")
        if command == "workspace":
            observed = wait_for(lambda: next((workspace for workspace in s.data("workspaces") if workspace["id"] == 8), None))
            expected = s.names[1 if plugin else 0]
            passed = observed["monitor"] == expected
        else:
            observed = wait_for(lambda: s.windows().get("hs-lua-tool"))
            expected = 12 if plugin else 11
            passed = observed["workspace"]["id"] == expected
        case.update(observed=observed, expected=expected, passed=passed, after=state(plugin))
        save()
        assert passed, case
        assert s.compositor.poll() is None
        s.check(f"Lua {phase} {command} uses the {'overview target' if plugin else 'native active monitor'}")

    def reload(suffix="", error=None):
        if str(s.plugin) in Path(f"/proc/{s.compositor_pid}/maps").read_text():
            s.close()
        count = s.reload_count()
        s.config.write_text(s.config_text() + suffix)
        s.ctl("reload")
        if error == "syntax":
            wait_for(lambda: bool(s.run("hyprctl", "configerrors")))
        else:
            wait_for(lambda: s.reload_count() > count)
            errors = s.run("hyprctl", "configerrors")
            assert ("hyprspace-test-runtime" in errors) if error else not errors, errors
        assert s.compositor.poll() is None
        evidence.setdefault("reloads", []).append({"error": error, "configerrors": s.run("hyprctl", "configerrors"), "count": s.reload_count()})
        save()

    try:
        for phase in ("startup", "reload-1", "reload-2"):
            if phase != "startup":
                reload()
            route(phase, "workspace")
            route(phase, "exec")
        # Invalid configurations must not poison the next valid Lua state.
        reload("local invalid = )\n", error="syntax")
        reload()
        route("syntax-recovery", "workspace")
        route("syntax-recovery", "exec")
        reload('error("hyprspace-test-runtime")\n', error="runtime")
        reload()
        route("runtime-recovery", "workspace")
        route("runtime-recovery", "exec")
        s.close()
        count = s.reload_count()
        s.config.write_text(s.config_text(plugin=False))
        s.ctl("plugin", "unload", str(s.plugin))
        wait_for(lambda: s.reload_count() > count)
        wait_for(lambda: str(s.plugin) not in Path(f"/proc/{s.compositor_pid}/maps").read_text())
        assert not (s.root / "hypr" / s.env["HYPRLAND_INSTANCE_SIGNATURE"] / "hyprspace.sock").exists()
        assert s.ctl("configerrors") == ""
        route("unloaded", "workspace", plugin=False)
        route("unloaded", "exec", plugin=False)
        evidence["status"] = "passed"
    except BaseException as error:
        evidence.update(status="failed", error=repr(error))
        raise
    finally:
        save()
