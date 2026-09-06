#!/usr/bin/env python3
"""Background compositor checks; use --visible to open test windows on the desktop."""

import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import shlex
import signal
import socket
import subprocess
import tempfile
import time

from background import BackgroundDisplay, stop_process_group

REPO = Path(__file__).resolve().parents[2]
PLUGIN = REPO / "build/hyprspace.so"
CLIENT = REPO / "test/integration/client.py"


class OutputUnavailable(RuntimeError):
    pass


def wait_for(predicate, timeout=6):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        result = predicate()
        if result:
            return result
        time.sleep(0.05)
    raise AssertionError("timed out waiting for compositor state")


def validate_runtime(root, saved, metadata, visible):
    keys = {
        "XDG_RUNTIME_DIR",
        "WAYLAND_DISPLAY",
        "HYPRLAND_INSTANCE_SIGNATURE",
        "DBUS_SESSION_BUS_ADDRESS",
    }
    if saved.keys() != keys or not all(
        isinstance(v, str) and v for v in saved.values()
    ):
        raise ValueError("Invalid isolated compositor environment")
    host_runtime = os.environ.get("XDG_RUNTIME_DIR")
    if (host_runtime and root == Path(host_runtime).resolve()) or Path(
        saved["XDG_RUNTIME_DIR"]
    ).resolve() != root:
        raise ValueError(
            "--runtime must name a private test directory, not the desktop runtime"
        )
    for key in ("HYPRLAND_INSTANCE_SIGNATURE", "DBUS_SESSION_BUS_ADDRESS"):
        if saved[key] == os.environ.get(key):
            raise ValueError(f"--runtime must not reuse the desktop's {key}")
    display = (root / saved["WAYLAND_DISPLAY"]).resolve()
    ipc = (
        root / "hypr" / saved["HYPRLAND_INSTANCE_SIGNATURE"] / ".socket.sock"
    ).resolve()
    if (
        display.parent != root
        or not ipc.is_relative_to(root)
        or not all(p.is_socket() for p in (display, ipc))
    ):
        raise ValueError(
            "Isolated compositor sockets must exist inside its private directory"
        )
    mode = "visible" if visible else "headless"
    if metadata != {"mode": mode, "outputs": [f"WAYLAND-{i+1}" for i in range(3)]}:
        raise ValueError(
            "Isolated session mode does not match; visible sessions require --visible"
        )


class Suite:
    def __init__(self, runtime=None, visible=False):
        self.processes = []
        self.checks = []
        self.owned = runtime is None
        self.connected = False
        self.visible = visible
        self.background = None
        self.names = [f"WAYLAND-{i+1}" for i in range(3)]
        self.root = (
            Path(runtime).resolve()
            if runtime
            else Path(tempfile.mkdtemp(prefix="hs-i."))
        )
        self.env = os.environ.copy()
        for name in (
            "DISPLAY",
            "WAYLAND_DISPLAY",
            "WAYLAND_SOCKET",
            "DBUS_SESSION_BUS_ADDRESS",
            "HYPRLAND_INSTANCE_SIGNATURE",
            "HL_INITIAL_WORKSPACE_TOKEN",
            "HYPRSPACE_LAUNCH_TOKEN",
            "XDG_ACTIVATION_TOKEN",
            "DESKTOP_STARTUP_ID",
        ):
            self.env.pop(name, None)
        self.env["XDG_RUNTIME_DIR"] = str(self.root)
        # Force libseat to an unavailable private socket. Even from a TTY, the
        # child must use the parent Wayland connection, never claim a real seat.
        self.env["LIBSEAT_BACKEND"] = "seatd"
        self.env["SEATD_SOCK"] = str(self.root / "no-seatd.sock")
        self.env["GDK_BACKEND"] = "wayland"
        self.env["HS_INPUT_LOG"] = str(self.root / "input.log")
        if runtime:
            # Validate saved sockets before creating files or connecting input.
            self.attach()
        for variable, directory in (
            ("XDG_CONFIG_HOME", "config"),
            ("XDG_CACHE_HOME", "cache"),
            ("XDG_DATA_HOME", "data"),
        ):
            path = self.root / directory
            path.mkdir(exist_ok=True)
            self.env[variable] = str(path)
        try:
            if not runtime:
                self.start()
        except BaseException:
            if self.connected:
                try:
                    (self.root / "output-failure.json").write_text(
                        json.dumps(self.data("monitors"), indent=2)
                    )
                except Exception:
                    pass
            self.finish()
            raise
        try:
            self.pointer = self.spawn(
                [str(REPO / "build/test-pointer")],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                text=True,
            )
            assert self.pointer.stdout.readline().strip() == "ready"
        except BaseException:
            self.finish()
            raise

    def attach(self):
        saved = json.loads((self.root / "env.json").read_text())
        metadata = json.loads((self.root / "session.json").read_text())
        validate_runtime(self.root, saved, metadata, self.visible)
        self.env.update(saved)
        self.names = metadata["outputs"]
        self.connected = True

    def spawn(self, command, **kwargs):
        kwargs.setdefault("stdout", subprocess.DEVNULL)
        kwargs.setdefault(
            "stderr", (self.root / f"process-{len(self.processes)}.log").open("w")
        )
        process = subprocess.Popen(command, env=self.env, **kwargs)
        self.processes.append(process)
        return process

    def run(self, *command):
        return subprocess.check_output(
            command, env=self.env, text=True, timeout=10
        ).strip()

    def ctl(self, *args):
        result = self.run("hyprctl", *args)
        if not result.startswith(("[", "{")) and any(
            word in result.lower()
            for word in (
                "invalid",
                "error",
                "couldn't",
                "could not",
                "crashed",
                "must be",
            )
        ):
            raise AssertionError((args, result))
        return result

    def data(self, name):
        return json.loads(self.ctl("-j", name))

    def request(self, message):
        path = (
            self.root
            / "hypr"
            / self.env["HYPRLAND_INSTANCE_SIGNATURE"]
            / "hyprspace.sock"
        )
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as connection:
            connection.settimeout(2)
            connection.connect(str(path))
            connection.sendall(message.encode())
            return connection.recv(65536).decode()

    def status(self):
        return json.loads(self.request("status"))

    def await_outputs(self, count):
        try:
            wait_for(
                lambda: len(
                    [m for m in self.data("monitors") if m["name"] in self.names]
                )
                == count,
                timeout=15,
            )
        except AssertionError as error:
            (self.root / "output-failure.json").write_text(
                json.dumps(self.data("monitors"), indent=2)
            )
            raise OutputUnavailable(
                f"nested backend did not configure {count} outputs; diagnostics: {self.root}"
            ) from error

    def start(self):
        self.root.chmod(0o700)
        if self.visible:
            parent = str(
                Path(os.environ["XDG_RUNTIME_DIR"]) / os.environ["WAYLAND_DISPLAY"]
            )
        else:
            self.background = BackgroundDisplay(self.root, self.env)
            parent = self.background.start()
        self.env["WAYLAND_DISPLAY"] = parent
        config = self.root / "hyprland.conf"
        config.write_text("""monitor = ,960x600@60,auto,1
misc:disable_hyprland_logo = true
misc:disable_splash_rendering = true
animations:enabled = false
input:follow_mouse = 0
input:resolve_binds_by_sym = true
device {
    name = wl_keyboard
    enabled = false
}
device {
    name = wl_pointer
    enabled = false
}
debug:disable_logs = false
bind = SUPER,J,layoutmsg,togglesplit
bindm = SUPER,mouse:272,movewindow
bindm = SUPER,mouse:273,resizewindow
""")
        boot = 'import os,sys; from pathlib import Path; Path(sys.argv[1]).write_text(os.environ["DBUS_SESSION_BUS_ADDRESS"]); os.execvp("Hyprland", ["Hyprland", "--config", sys.argv[2]])'
        log = (self.root / "compositor.log").open("w")
        self.compositor = subprocess.Popen(
            [
                "dbus-run-session",
                "--",
                "python3",
                "-c",
                boot,
                str(self.root / "dbus"),
                str(config),
            ],
            env=self.env,
            stdout=log,
            stderr=log,
            start_new_session=True,
        )

        def ready():
            if self.compositor.poll() is not None:
                raise RuntimeError(
                    f"Test compositor exited; see {self.root / 'compositor.log'}"
                )
            return list((self.root / "hypr").glob("*/.socket.sock"))

        wait_for(ready, timeout=15)
        instance = next((self.root / "hypr").glob("*/.socket.sock")).parent.name
        display = next(
            p for p in self.root.glob("wayland-*") if not p.name.endswith(".lock")
        )
        self.env.update(
            HYPRLAND_INSTANCE_SIGNATURE=instance,
            WAYLAND_DISPLAY=display.name,
            DBUS_SESSION_BUS_ADDRESS=(self.root / "dbus").read_text(),
        )
        self.connected = True
        (self.root / "env.json").write_text(
            json.dumps(
                {
                    k: self.env[k]
                    for k in (
                        "XDG_RUNTIME_DIR",
                        "WAYLAND_DISPLAY",
                        "HYPRLAND_INSTANCE_SIGNATURE",
                        "DBUS_SESSION_BUS_ADDRESS",
                    )
                }
            )
        )
        (self.root / "session.json").write_text(
            json.dumps(
                {
                    "mode": "visible" if self.visible else "headless",
                    "outputs": self.names,
                }
            )
        )
        self.await_outputs(1)
        self.ctl("output", "create", "wayland")
        self.await_outputs(2)
        self.ctl("output", "create", "wayland")
        self.await_outputs(3)
        if self.visible:
            self.start_visible_outputs()
        else:
            # The private host lays out three unoccluded surfaces in memory.
            wait_for(
                lambda: all(
                    (m["width"], m["height"]) == (960, 600)
                    for m in self.data("monitors")
                )
            )
        with config.open("a") as output:
            output.write(self.monitor_rules())
        self.ctl("plugin", "load", str(PLUGIN))
        # Register plugin bindings only after its dispatchers exist. An initial
        # parse error reserves a transient error-bar strip and changes geometry.
        with config.open("a") as output:
            output.write(
                "bind = SUPER,A,hyprspace:overview\nbind = SUPER,L,hyprspace:layoutcycle\n"
            )
        self.ctl("reload")
        wait_for(lambda: len(self.status()["views"]) == 0)
        wait_for(
            lambda: {m["name"]: m["scale"] for m in self.data("monitors")}
            == dict(zip(self.names, (1, 1.25, 1.5)))
        )
        assert self.ctl("configerrors") == ""
        self.check("plugin loads with exact ABI and clean configuration")

    def monitor_rules(self):
        return (
            f"monitor = {self.names[0]},960x600@60,0x0,1\n"
            f"monitor = {self.names[1]},960x600@60,-1000x-200,1.25,transform,1\n"
            f"monitor = {self.names[2]},960x600@60,2200x100,1.5\n"
        )

    def start_visible_outputs(self):
        assert self.visible, "Desktop windows require --visible"
        # Keep all disposable outputs visible on the host. Fully occluded
        # Wayland windows stop receiving frames, which also stalls teardown.
        pid = int(
            next((self.root / "hypr").glob("*/hyprland.lock"))
            .read_text()
            .splitlines()[0]
        )

        def host_windows():
            return [
                window
                for window in json.loads(
                    subprocess.check_output(["hyprctl", "-j", "clients"], text=True)
                )
                if window["pid"] == pid
            ]

        wait_for(lambda: len(host_windows()) == 3)
        monitors = json.loads(
            subprocess.check_output(["hyprctl", "-j", "monitors"], text=True)
        )
        for index, window in enumerate(host_windows()):
            monitor = monitors[index % len(monitors)]
            offset = 24 + 80 * (index // len(monitors))
            x = monitor["x"] + monitor["reserved"][0] + offset
            y = monitor["y"] + monitor["reserved"][1] + offset
            for dispatch, args in [
                ("setfloating", "address:" + window["address"]),
                ("movewindowpixel", f'exact {x} {y},address:{window["address"]}'),
                ("resizewindowpixel", "exact 960 600,address:" + window["address"]),
            ]:
                subprocess.run(
                    ["hyprctl", "dispatch", dispatch, args],
                    check=True,
                    stdout=subprocess.DEVNULL,
                )
        try:
            wait_for(
                lambda: all(
                    (monitor["width"], monitor["height"]) == (960, 600)
                    for monitor in self.data("monitors")
                )
            )
        except AssertionError as error:
            raise OutputUnavailable(
                "nested outputs did not acknowledge their host window sizes"
            ) from error

    def check(self, name):
        print("PASS", name, flush=True)
        self.checks.append(name)

    def windows(self):
        return {
            w["title"]: w for w in self.data("clients") if w["title"].startswith("hs-")
        }

    def close(self):
        self.ctl("dispatch", "hyprspace:close")
        wait_for(lambda: not self.status()["live"])
        # Input is released before the closing views finish rendering. Await
        # that teardown before asserting visibility or starting another case.
        wait_for(lambda: not self.status()["views"])

    def setup(self, layout, source, destination):
        self.close()
        for pid in {window["pid"] for window in self.windows().values()}:
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        for process in self.processes[1:]:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
        self.processes = self.processes[:1]
        wait_for(lambda: not self.windows())
        for i in range(3):
            ws = 11 + i
            self.ctl(
                "keyword",
                "workspace",
                f"{ws},monitor:{self.names[i]},persistent:true,layout:{layout}",
            )
            self.ctl("dispatch", "focusmonitor", self.names[i])
            self.ctl("dispatch", "workspace", str(ws))
        self.ctl("dispatch", "focusmonitor", self.names[destination])
        monitor = next(
            m for m in self.data("monitors") if m["name"] == self.names[destination]
        )
        self.move(
            (
                monitor["x"] + monitor["reserved"][0] + 30,
                monitor["y"] + monitor["reserved"][1] + 30,
            )
        )
        # Dwindle insertion depends on pointer position and map order. Seed both
        # so background scheduling cannot give native and overview different trees.
        for count, title in enumerate(("hs-A", "hs-B", "hs-C"), 1):
            self.spawn(["python3", str(CLIENT), title])
            wait_for(lambda: len(self.windows()) == count)
            self.ctl(
                "dispatch", "focuswindow", "address:" + self.windows()[title]["address"]
            )
            self.move(self.point(self.windows()[title], 0.5, 0.5))
        for title in ("hs-B", "hs-C"):
            self.ctl(
                "dispatch",
                "movetoworkspacesilent",
                f'{11+destination},address:{self.windows()[title]["address"]}',
            )
        self.ctl(
            "dispatch",
            "movetoworkspacesilent",
            f'{11+source},address:{self.windows()["hs-A"]["address"]}',
        )
        self.ctl(
            "dispatch", "focuswindow", "address:" + self.windows()["hs-A"]["address"]
        )
        time.sleep(0.1)

    def move(self, point):
        self.ctl("--batch", f"dispatch movecursor {point[0]:.0f} {point[1]:.0f}")
        # hyprctl's warp emits motion without a frame. Flush it as a device
        # would so GTK processes hover before the subsequent button frame.
        self.button(0, 0)
        time.sleep(0.04)

    def button(self, state, button=272):
        self.pointer.stdin.write(f"{button} {state}\n")
        self.pointer.stdin.flush()
        assert self.pointer.stdout.readline().strip() == "ok"

    def scroll(self, delta=15, discrete=1, axis=0, source=0):
        self.pointer.stdin.write(f"axis {axis} {delta} {discrete} {source}\n")
        self.pointer.stdin.flush()
        assert self.pointer.stdout.readline().strip() == "ok"

    def drag(self, source, destination, overview, cancel=False, button=272):
        self.move(source)
        self.key(125, 1)
        pressed = False
        try:
            self.button(1, button)
            pressed = True
            if overview:
                assert self.status()["dragging"]
            self.move(destination)
            if cancel:
                self.run("wtype", "-k", "Escape")
            self.button(0, button)
            pressed = False
        finally:
            if pressed:
                self.button(0, button)
            self.key(125, 0)
        if overview:
            assert self.status()["live"]
            assert not self.status()["dragging"]
        time.sleep(0.08)

    def key(self, code, state):
        self.pointer.stdin.write(f"key {code} {state}\n")
        self.pointer.stdin.flush()
        assert self.pointer.stdout.readline().strip() == "ok"

    @staticmethod
    def point(window, x=0.35, y=0.4):
        return (
            window["at"][0] + window["size"][0] * x,
            window["at"][1] + window["size"][1] * y,
        )

    def preview(self, title):
        address = self.windows()[title]["address"]
        return next(
            tile
            for view in self.status()["views"]
            for tile in view["tiles"]
            if tile["window"] == address
        )

    def preview_point(self, title, x=0.35, y=0.4):
        tile = self.preview(title)
        return (tile["x"] + tile["w"] * x, tile["y"] + tile["h"] * y)

    def geometry(self):
        assert self.status()[
            "layout_targets_unique"
        ], "duplicate native layout membership"
        windows = self.windows()
        titles = {w["address"]: title for title, w in windows.items()}
        return {
            title: (
                w["workspace"]["id"],
                w["floating"],
                w["at"],
                w["size"],
                w["fullscreen"],
                w["fullscreenClient"],
                sorted(titles.get(address, address) for address in w["grouped"]),
            )
            for title, w in windows.items()
        }

    def layer(self, namespace):
        return next(
            (
                layer
                for monitor in self.data("layers").values()
                for level in monitor["levels"].values()
                for layer in level
                if layer["namespace"] == namespace
            ),
            None,
        )

    def foreground(self):
        self.setup("dwindle", 0, 1)
        self.ctl("dispatch", "hyprspace:overview", "on")
        time.sleep(0.15)
        self.move(self.preview_point("hs-A"))
        entry = self.root / "entry"
        foreground = self.spawn(
            ["python3", str(REPO / "test/integration/layer.py"), str(entry)]
        )
        wait_for(lambda: self.layer("hs-foreground"))
        self.move(self.preview_point("hs-B"))
        assert self.status()["target"]["workspace"] == 12
        self.run("wtype", "-M", "logo", "-k", "j", "-m", "logo")
        assert self.data("activewindow")["address"] == self.windows()["hs-B"]["address"]
        self.run("wtype", "-M", "logo", "-k", "l", "-m", "logo")
        assert (
            next(w for w in self.data("workspaces") if w["id"] == 12)["tiledLayout"]
            == "scrolling"
        )
        assert not self.status()["keyboard_owned"]
        self.run("wtype", "foreground typing")
        wait_for(lambda: entry.exists() and entry.read_text() == "foreground typing")
        box = self.layer("hs-foreground")
        self.move((box["x"] + 70, box["y"] + 70))
        self.button(1)
        self.button(0)
        assert self.status()["live"] and self.status()["target"]["workspace"] == 12
        self.check(
            "foreground keyboard focus survives pointer targeting and native layer clicks"
        )
        self.run(
            "grim", "-s", "1", "-o", self.names[0], str(self.root / "foreground.png")
        )
        from PIL import Image, ImageChops

        with Image.open(self.root / "foreground.png") as screenshot:
            pixel = screenshot.convert("RGB").getpixel((box["x"] + 5, box["y"] + 5))
            assert (
                abs(pixel[0] - 224) < 4
                and abs(pixel[1] - 64) < 4
                and abs(pixel[2] - 128) < 4
            ), pixel
        self.check("foreground layers render above the overview")
        self.run("wtype", "-k", "Escape")
        foreground.wait(timeout=3)
        wait_for(lambda: not self.layer("hs-foreground"))
        assert self.status()["live"]
        wait_for(lambda: self.status()["cursor_owned"])
        self.check("Escape dismisses foreground UI before the overview")
        self.move(self.preview_point("hs-A"))
        x, y = self.preview_point("hs-A")
        self.run(
            "grim",
            "-s",
            "1",
            "-c",
            "-o",
            self.names[0],
            str(self.root / "cursor-first.png"),
        )
        self.move(self.preview_point("hs-A", 0.7, 0.4))
        self.run(
            "grim",
            "-s",
            "1",
            "-c",
            "-o",
            self.names[0],
            str(self.root / "cursor-second.png"),
        )
        with Image.open(self.root / "cursor-first.png") as plain, Image.open(
            self.root / "cursor-second.png"
        ) as cursor:
            region = (int(x) - 8, int(y) - 8, int(x) + 64, int(y) + 64)
            assert ImageChops.difference(
                plain.convert("RGB").crop(region), cursor.convert("RGB").crop(region)
            ).getbbox()
        self.check("software cursor is visible above overview previews")
        foreground = self.spawn(
            ["python3", str(REPO / "test/integration/layer.py"), str(entry)]
        )
        wait_for(lambda: self.layer("hs-foreground"))
        foreground.kill()
        foreground.wait(timeout=3)
        wait_for(lambda: not self.layer("hs-foreground"))
        self.run("wtype", "-k", "Escape")
        wait_for(lambda: not self.status()["live"])
        self.check("foreground crash restores overview keyboard ownership")
        self.ctl(
            "keyword",
            "bind",
            f',Print,exec,python3 {shlex.quote(str(REPO/"test/integration/layer.py"))} {shlex.quote(str(entry))} slurp',
        )
        self.ctl("dispatch", "hyprspace:overview", "on")
        time.sleep(0.15)
        self.run("wtype", "-k", "Print")
        wait_for(lambda: self.layer("slurp"))
        self.run("wtype", "selector")
        wait_for(lambda: entry.read_text() == "selector")
        self.run("wtype", "-k", "Escape")
        wait_for(lambda: not self.layer("slurp"))
        wait_for(lambda: self.status()["keyboard_owned"])
        assert self.status()["live"]
        self.check(
            "Print hands input to a screenshot selector and restores the overview afterward"
        )
        panel = self.spawn(
            [
                "python3",
                str(REPO / "test/integration/layer.py"),
                str(entry),
                "waybar",
                "bottom",
            ]
        )
        wait_for(lambda: self.layer("waybar"))
        time.sleep(0.2)
        box = self.layer("waybar")
        click = Path(str(entry) + ".click")
        click.unlink(missing_ok=True)
        self.move((box["x"] + 70, box["y"] + box["h"] / 2))
        self.button(1)
        time.sleep(0.05)
        self.button(0)
        wait_for(click.exists)
        self.run(
            "grim", "-s", "1", "-o", self.names[0], str(self.root / "bottom-panel.png")
        )
        with Image.open(self.root / "bottom-panel.png") as screenshot:
            pixel = screenshot.convert("RGB").getpixel(
                (box["x"] + 5, box["y"] + box["h"] - 10)
            )
            assert (
                abs(pixel[0] - 224) < 4
                and abs(pixel[1] - 64) < 4
                and abs(pixel[2] - 128) < 4
            ), pixel
        assert self.status()["live"]
        panel.terminate()
        panel.wait(timeout=3)
        self.check(
            "Waybar on a reserved bottom layer stays visible and receives native pointer clicks"
        )

    @staticmethod
    def protocol(process, message):
        process.stdin.write(message + "\n")
        process.stdin.flush()
        return process.stdout.readline().strip()

    def activation(self):
        from regressions import events, offsets

        self.env["WAYLAND_DEBUG"] = "client"
        try:
            self.setup("dwindle", 0, 1)
        finally:
            self.env.pop("WAYLAND_DEBUG")
        app = self.spawn(
            [str(REPO / "build/test-activation")],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
        )
        assert app.stdout.readline().strip() == "ready"
        assert self.protocol(app, "after hs-existing -") == "ok"
        wait_for(lambda: "hs-existing" in self.windows())
        self.ctl(
            "dispatch",
            "movetoworkspacesilent",
            f'12,address:{self.windows()["hs-existing"]["address"]}',
        )
        self.ctl("dispatch", "hyprspace:overview", "on")
        time.sleep(0.15)
        for mode in ("before", "after"):
            self.move(self.preview_point("hs-A", 0.45, 0.45))
            token = self.request("consume " + self.request("capture"))
            assert token
            self.move(self.preview_point("hs-B", 0.4, 0.4))
            title = "hs-" + mode
            assert self.protocol(app, f"{mode} {title} {token}") == "ok"
            wait_for(
                lambda: title in self.windows()
                and self.windows()[title]["workspace"]["id"] == 11
            )
            assert self.windows()["hs-existing"]["workspace"]["id"] == 12
            self.check(
                f"XDG activation correlates a new surface {mode} mapping in an existing process"
            )
        self.move(self.preview_point("hs-A", 0.55, 0.5))
        token = self.request("consume " + self.request("capture"))
        before = {title: w["workspace"]["id"] for title, w in self.windows().items()}
        assert self.protocol(app, "activate 0 " + token) == "ok"
        assert {
            title: w["workspace"]["id"] for title, w in self.windows().items()
        } == before
        self.check("existing-window activation retains native placement")
        self.ctl("keyword", "windowrule", "match:title ^hs-rule$, workspace 13 silent")
        self.move(self.preview_point("hs-A", 0.55, 0.5))
        token = self.request("consume " + self.request("capture"))
        assert self.protocol(app, "after hs-rule " + token) == "ok"
        wait_for(lambda: "hs-rule" in self.windows())
        assert self.windows()["hs-rule"]["workspace"]["id"] == 13
        self.check("explicit window workspace rules override launch context")
        self.move(self.preview_point("hs-A", 0.6, 0.5))
        pending = self.request("capture")
        saved = offsets(self)
        self.protocol(app, "lock")
        wait_for(lambda: not self.status()["live"])
        self.run(
            "wtype", "-M", "ctrl", "-M", "shift", "-k", "q", "-m", "shift", "-m", "ctrl"
        )
        assert not events(saved, r"wl_keyboard#\d+\.(enter|key)\(")
        assert not events(saved, r"wl_keyboard#\d+\.modifiers\(\d+, [1-9]")
        assert self.request("consume " + pending) == ""
        assert all(window["alpha"] == 1 for window in self.status()["windows"])
        self.check(
            "session lock immediately closes the overview, restores visibility and clears launch contexts"
        )

    def browser(self):
        self.setup("dwindle", 0, 1)
        destination = self.windows()["hs-A"]["workspace"]["id"]
        original = self.windows()["hs-B"]["workspace"]["id"]
        empty_workspace = next(
            monitor["activeWorkspace"]["id"]
            for monitor in self.data("monitors")
            if monitor["activeWorkspace"]["id"] not in (destination, original)
        )
        existing = {w["address"] for w in self.data("clients")}
        profile = self.root / "firefox-profile"
        profile.mkdir(exist_ok=True)
        (profile / "user.js").write_text(
            'user_pref("browser.aboutwelcome.enabled", false);\nuser_pref("browser.shell.checkDefaultBrowser", false);\nuser_pref("browser.startup.homepage_override.mstone", "ignore");\n'
        )
        self.env["MOZ_ENABLE_WAYLAND"] = "1"
        self.env["MOZ_DBUS_REMOTE"] = "1"
        self.spawn(
            ["firefox", "--profile", str(profile), "--new-window", "about:blank"]
        )

        def browsers():
            return {
                w["address"]: w
                for w in self.data("clients")
                if "firefox" in w["class"].lower() and w["address"] not in existing
            }

        first = wait_for(lambda: browsers(), timeout=20)
        assert len(first) == 1
        for address in first:
            self.ctl(
                "dispatch", "movetoworkspacesilent", f"{original},address:{address}"
            )
        self.ctl("dispatch", "hyprspace:overview", "on")
        time.sleep(0.2)
        self.move(self.preview_point("hs-A"))
        token = self.request("capture")
        self.move(self.preview_point("hs-B"))
        self.spawn(
            [
                str(REPO / "build/hyprspace-launch"),
                "--context",
                token,
                "--",
                "firefox",
                "--profile",
                str(profile),
                "--new-window",
                "about:blank",
            ]
        )
        wait_for(lambda: len(browsers()) == 2, timeout=15)
        new = next(w for address, w in browsers().items() if address not in first)
        assert new["workspace"]["id"] == destination, new
        assert all(
            browsers()[address]["workspace"]["id"] == original for address in first
        )
        assert len({w["pid"] for w in browsers().values()}) == 1
        self.check(
            "Firefox correlates a new window through activation in its existing process"
        )
        empty = next(
            tile
            for view in self.status()["views"]
            for tile in view["tiles"]
            if tile["workspace"] == empty_workspace and tile["window"] == "0x0"
        )
        self.move((empty["x"] + empty["w"] * 0.4, empty["y"] + empty["h"] * 0.4))
        token = self.request("capture")
        before = {address: w["workspace"]["id"] for address, w in browsers().items()}
        self.run(
            str(REPO / "build/hyprspace-launch"),
            "--context",
            token,
            "--",
            "firefox",
            "--profile",
            str(profile),
            "--new-tab",
            "about:blank",
        )
        time.sleep(0.6)
        assert before == {
            address: w["workspace"]["id"] for address, w in browsers().items()
        }
        assert self.status()["live"] and self.status()["layout_targets_unique"]
        self.check("Firefox existing-window reuse retains native workspace placement")

    def discord(self, binary):
        self.setup("dwindle", 0, 1)
        original = self.windows()["hs-B"]["workspace"]["id"]
        existing = {w["address"] for w in self.data("clients")}
        # Discord stores its updater and profile below XDG_CONFIG_HOME too.
        # Keep every invocation in this test's private application directories.
        previous = self.env.copy()
        try:
            audio = Path(os.environ["XDG_RUNTIME_DIR"]) / "pulse/native"
            if "PULSE_SERVER" not in self.env and audio.is_socket():
                # A nested compositor has a private runtime directory, but
                # Discord still initializes audio before showing its login UI.
                self.env["PULSE_SERVER"] = "unix:" + str(audio)
            pipewire = Path(os.environ["XDG_RUNTIME_DIR"]) / "pipewire-0"
            if self.owned and pipewire.is_socket():
                (self.root / "pipewire-0").symlink_to(pipewire)
            for variable, directory in (
                ("XDG_CONFIG_HOME", "discord-config"),
                ("XDG_CACHE_HOME", "discord-cache"),
                ("XDG_DATA_HOME", "discord-data"),
                ("TMPDIR", "discord-tmp"),
            ):
                path = self.root / directory
                path.mkdir(exist_ok=True)
                self.env[variable] = str(path)
            command = [
                str(binary.resolve()),
                "--ozone-platform=wayland",
                "--user-data-dir=" + str(self.root / "discord-profile"),
            ]
            self.spawn(command, stdout=(self.root / "discord.log").open("w"))

            def windows():
                return {
                    w["address"]: w
                    for w in self.data("clients")
                    if "discord" in w["class"].lower()
                    and w["address"] not in existing
                    and w["title"] != "Discord Updater"
                }

            # Establish a mapped main window before capturing a destination.
            # A second ordinary invocation can reveal it while the updater
            # splash is still waiting for the signed-out web UI to initialize.
            wait_for(
                lambda: 'window.created win2 "Discord"'
                in (self.root / "discord.log").read_text(),
                timeout=45,
            )
            assert self.spawn(command).wait(timeout=15) == 0
            first = wait_for(lambda: windows(), timeout=45)
            for address in first:
                self.ctl(
                    "dispatch", "movetoworkspacesilent", f"{original},address:{address}"
                )
            # Wait for the mapped main window to settle before capturing.
            time.sleep(1)
            before = windows()
            assert before and all(
                w["workspace"]["id"] == original for w in before.values()
            )
            self.ctl("dispatch", "hyprspace:overview", "on")
            time.sleep(0.2)
            self.move(self.preview_point("hs-A"))
            token = self.request("capture")
            assert token
            process = self.spawn(
                [
                    str(REPO / "build/hyprspace-launch"),
                    "--context",
                    token,
                    "--",
                    *command,
                ]
            )
            assert process.wait(timeout=15) == 0
            time.sleep(0.6)
            (self.root / "discord-windows.json").write_text(
                json.dumps({"before": before, "after": windows()}, indent=2)
            )
            assert {
                address: (w["workspace"]["id"], w["pid"])
                for address, w in windows().items()
            } == {
                address: (w["workspace"]["id"], w["pid"])
                for address, w in before.items()
            }
            assert self.status()["live"] and self.status()["layout_targets_unique"]
            self.check(
                "Discord existing-process window reuse retains native workspace placement"
            )
        finally:
            self.env = previous

    def companions(self, directory):
        directory = directory.resolve()
        assert (directory / "walker").is_file() and (directory / "elephant").is_file()
        record = json.loads((directory / "compatibility.json").read_text())
        versions = json.loads((REPO / "companion/versions.json").read_text())
        for name, spec in versions.items():
            assert all(record[name][key] == value for key, value in spec.items())
            patch = REPO / f'companion/{name}-{spec["version"]}.patch'
            assert (
                record[name]["patch_sha256"]
                == hashlib.sha256(patch.read_bytes()).hexdigest()
            )
        for name, digest in record["binaries"].items():
            assert hashlib.sha256((directory / name).read_bytes()).hexdigest() == digest
        self.check(
            "companion versions, patches and binaries match the compatibility record"
        )
        self.setup("dwindle", 0, 1)
        bins = self.root / "bin"
        bins.mkdir(exist_ok=True)
        for name in ("hs-walker-one", "hs-walker-two", "hs-walker-mouse"):
            script = bins / name
            script.write_text(
                f"#!/bin/sh\nsleep .25\nexec python3 {shlex.quote(str(CLIENT))} {name}\n"
            )
            script.chmod(0o755)
        self.env["PATH"] = f'{bins}:{directory}:{REPO / "build"}:' + self.env["PATH"]
        self.env["GTK_A11Y"] = "none"
        self.env["ELEPHANT_PROVIDER_DIR"] = str(directory / "providers")
        config = self.root / "config/elephant"
        config.mkdir(parents=True, exist_ok=True)
        (config / "elephant.toml").write_text("auto_detect_launch_prefix = false\n")
        (config / "runner.toml").write_text(
            "history = false\n"
            + "".join(
                f'[[explicits]]\nexec = "{name}"\nalias = "{name}"\n'
                for name in ("hs-walker-one", "hs-walker-two", "hs-walker-mouse")
            )
        )
        walkerconfig = self.root / "config/walker"
        walkerconfig.mkdir(parents=True, exist_ok=True)
        (walkerconfig / "config.toml").write_text("""[providers.actions]
runner = [
 { action = "run", default = true, bind = "Return" },
 { action = "run:keep", bind = "ctrl Return", after = "ClearReload" },
]
""")
        elephant = self.spawn(
            [str(directory / "elephant"), "--config", str(config), "--debug"]
        )
        wait_for(lambda: (self.root / "elephant/elephant.sock").exists())
        self.ctl("dispatch", "hyprspace:overview", "on")
        time.sleep(0.15)
        self.move(self.preview_point("hs-A"))
        walker = self.spawn(
            [
                str(directory / "walker"),
                "-m",
                "runner",
                "--width",
                "360",
                "--height",
                "260",
            ]
        )
        wait_for(lambda: self.layer("walker"))
        wait_for(lambda: not self.status()["keyboard_owned"])
        self.move(self.preview_point("hs-B"))
        assert self.status()["target"]["workspace"] == 12
        self.run("wtype", "hs-walker-one")
        time.sleep(0.4)
        self.run("wtype", "-M", "ctrl", "-k", "Return", "-m", "ctrl")
        self.move(self.preview_point("hs-A", 0.05, 0.8))
        wait_for(lambda: "hs-walker-one" in self.windows())
        assert self.windows()["hs-walker-one"]["workspace"]["id"] == 12
        assert self.layer("walker") and self.status()["live"]
        self.check(
            "Walker keep-open activation freezes its per-request destination through Elephant runner"
        )
        assert self.status()["target"]["workspace"] == 11
        self.run("wtype", "hs-walker-two")
        time.sleep(0.3)
        self.run("wtype", "-k", "Return")
        self.move(self.preview_point("hs-B", 0.55, 0.5))
        wait_for(lambda: "hs-walker-two" in self.windows())
        assert self.windows()["hs-walker-two"]["workspace"]["id"] == 11
        wait_for(lambda: not self.layer("walker"))
        assert self.status()["live"]
        self.check(
            "Walker keyboard activation launches at the selected workspace and keeps overview open"
        )
        if walker.poll() is None:
            walker.terminate()
            walker.wait(timeout=3)
        self.move(self.preview_point("hs-A", 0.6, 0.5))
        walker = self.spawn(
            [
                str(directory / "walker"),
                "-m",
                "runner",
                "--width",
                "360",
                "--height",
                "260",
            ]
        )
        wait_for(lambda: self.layer("walker"))
        wait_for(lambda: not self.status()["keyboard_owned"])
        self.run("wtype", "hs-walker-mouse")
        time.sleep(0.4)
        self.move(self.preview_point("hs-B", 0.5, 0.6))
        assert self.status()["target"]["workspace"] == 12
        box = self.layer("walker")
        # The native layer spans the output; its 260px panel is centred.
        # Walker re-enables mouse selection after two distinct motions.
        self.move((box["x"] + box["w"] / 2, box["y"] + (box["h"] - 260) / 2 + 40))
        self.move((box["x"] + box["w"] / 2, box["y"] + (box["h"] - 260) / 2 + 100))
        self.button(1)
        time.sleep(0.05)
        self.button(0)
        wait_for(lambda: "hs-walker-mouse" in self.windows())
        assert self.windows()["hs-walker-mouse"]["workspace"]["id"] == 12
        assert self.status()["live"]
        self.check(
            "Walker mouse result activation retains the last overview destination"
        )

    def matrix(self, quick):
        pairs = (
            [(0, 1), (1, 2), (2, 0), (0, 0)]
            if quick
            else list(itertools.product(range(3), repeat=2))
        )
        for layout in ("dwindle", "scrolling", "master"):
            for source, destination in pairs:
                self.setup(layout, source, destination)
                initial = self.geometry()
                src = self.point(self.windows()["hs-A"])
                dst = self.point(self.windows()["hs-B"], 0.25, 0.45)
                self.drag(src, dst, False)
                native = self.geometry()
                assert native["hs-A"][0] == 11 + destination, (
                    "native gesture did not reach destination",
                    layout,
                    source,
                    destination,
                    native,
                )
                self.setup(layout, source, destination)
                assert self.geometry() == initial, (
                    "native and overview fixtures must start with identical layouts",
                    layout,
                    source,
                    destination,
                    initial,
                    self.geometry(),
                )
                self.ctl("dispatch", "hyprspace:overview", "on")
                time.sleep(0.15)
                self.drag(
                    self.preview_point("hs-A"),
                    self.preview_point("hs-B", 0.25, 0.45),
                    True,
                )
                actual = self.geometry()
                if actual != native:
                    (self.root / f"{layout}-{source}-{destination}.json").write_text(
                        json.dumps({"native": native, "overview": actual}, indent=2)
                    )
                assert actual == native, (layout, source, destination, native, actual)
                self.close()
                assert all(w["alpha"] == 1 for w in self.status()["windows"])
                self.check(
                    f"{layout}: native-equivalent drag {source+1} -> {destination+1}"
                )

    def special(self):
        for layout in ("dwindle", "scrolling", "master"):
            for mode in (
                "floating-move",
                "floating-resize",
                "tiled-resize",
                "fullscreen",
                "maximized",
                "grouped",
            ):
                outcomes = []
                for overview in (False, True):
                    self.setup(layout, 0, 1)
                    address = self.windows()["hs-A"]["address"]
                    if mode.startswith("floating"):
                        self.ctl("dispatch", "setfloating", "address:" + address)
                        self.ctl(
                            "dispatch",
                            "resizewindowpixel",
                            "exact 320 240,address:" + address,
                        )
                        self.ctl(
                            "dispatch",
                            "movewindowpixel",
                            "exact 100 100,address:" + address,
                        )
                    if mode in ("fullscreen", "maximized"):
                        self.ctl(
                            "dispatch",
                            "fullscreen",
                            "0" if mode == "fullscreen" else "1",
                        )
                    if mode == "grouped":
                        self.ctl("dispatch", "togglegroup")
                        self.ctl(
                            "dispatch",
                            "movetoworkspacesilent",
                            f'11,address:{self.windows()["hs-C"]["address"]}',
                        )
                        self.ctl(
                            "dispatch",
                            "focuswindow",
                            "address:" + self.windows()["hs-C"]["address"],
                        )
                        first, second = self.point(self.windows()["hs-A"]), self.point(
                            self.windows()["hs-C"]
                        )
                        dx, dy = first[0] - second[0], first[1] - second[1]
                        direction = (
                            ("l" if dx < 0 else "r")
                            if abs(dx) > abs(dy)
                            else ("u" if dy < 0 else "d")
                        )
                        self.ctl("dispatch", "moveintogroup", direction)
                        self.ctl("dispatch", "focuswindow", "address:" + address)
                        assert len(self.windows()["hs-A"]["grouped"]) >= 2
                    if overview:
                        self.ctl("dispatch", "hyprspace:overview", "on")
                        time.sleep(0.15)
                        source = self.preview_point("hs-A", 0.7, 0.7)
                        destination = (
                            (source[0] + 25, source[1] + 20)
                            if "resize" in mode
                            else self.preview_point("hs-B", 0.25, 0.45)
                        )
                    else:
                        source = self.point(self.windows()["hs-A"], 0.7, 0.7)
                        if "resize" in mode:
                            # Match the miniature's desktop delta exactly.
                            self.ctl("dispatch", "hyprspace:overview", "on")
                            time.sleep(0.15)
                            scale = (
                                self.preview("hs-A")["w"]
                                / self.windows()["hs-A"]["size"][0]
                            )
                            self.close()
                            destination = (
                                source[0] + 25 / scale,
                                source[1] + 20 / scale,
                            )
                        else:
                            destination = self.point(self.windows()["hs-B"], 0.25, 0.45)
                    self.drag(
                        source,
                        destination,
                        overview,
                        button=273 if "resize" in mode else 272,
                    )
                    outcomes.append(self.geometry())

                def close_geometry(a, b):
                    return a.keys() == b.keys() and all(
                        a[k][:2] == b[k][:2]
                        and a[k][4:] == b[k][4:]
                        and all(
                            abs(x - y) <= 3
                            for pair in zip(a[k][2:4], b[k][2:4])
                            for x, y in zip(*pair)
                        )
                        for k in a
                    )

                assert close_geometry(*outcomes), (layout, mode, outcomes)
                if mode == "floating-move":
                    assert outcomes[1]["hs-A"][0] == 12 and outcomes[1]["hs-A"][3] == [
                        320,
                        240,
                    ]
                self.check(f"{layout}: native-equivalent {mode}")

    def keyboard(self):
        self.setup("dwindle", 0, 1)
        self.run("wtype", "-M", "logo", "-k", "a", "-m", "logo")
        wait_for(lambda: self.status()["live"])
        self.run("wtype", "-M", "logo", "-k", "a", "-m", "logo")
        wait_for(lambda: not self.status()["live"])
        self.check("Super+A toggles the overview through the native matcher")
        self.ctl("dispatch", "hyprspace:overview", "on")
        time.sleep(0.15)
        for binding, title, delay in [
            ("SUPER,B", "hs-direct-b", 0.6),
            ("SUPER SHIFT,A", "hs-direct-shift-a", 0.2),
        ]:
            self.ctl(
                "keyword",
                "bind",
                f"{binding},exec,sleep {delay}; exec python3 {shlex.quote(str(CLIENT))} {title}",
            )
        self.move(self.preview_point("hs-A"))
        self.run("wtype", "-M", "logo", "-k", "b", "-m", "logo")
        self.move(self.preview_point("hs-B"))
        self.run(
            "wtype", "-M", "logo", "-M", "shift", "-k", "a", "-m", "shift", "-m", "logo"
        )
        self.move(self.preview_point("hs-A", 0.6, 0.5))
        wait_for(
            lambda: "hs-direct-b" in self.windows()
            and "hs-direct-shift-a" in self.windows()
        )
        assert self.windows()["hs-direct-b"]["workspace"]["id"] == 11
        assert self.windows()["hs-direct-shift-a"]["workspace"]["id"] == 12
        assert self.status()["live"] and self.status()["layout_targets_unique"]
        self.check(
            "Super+B and Super+Shift+A retain separate destinations for concurrent delayed launches"
        )
        self.ctl("keyword", "bind", "CTRL,F,cyclenext")
        self.move(self.preview_point("hs-B"))
        self.run("wtype", "-M", "ctrl", "-k", "f", "-m", "ctrl")
        selected = self.data("activewindow")["address"]
        assert selected != self.windows()["hs-B"]["address"]
        assert self.status()["target"]["window"] == selected
        self.run("wtype", "-M", "logo", "-k", "j", "-m", "logo")
        assert self.data("activewindow")["address"] == selected
        self.move(self.preview_point("hs-B", 0.5, 0.5))
        assert self.status()["target"]["window"] == self.windows()["hs-B"]["address"]
        self.check("native keyboard focus navigation persists until pointer motion")
        release, submap = [self.root / name for name in ("release", "submap")]
        self.ctl("keyword", "input:repeat_delay", "150")
        self.ctl("keyword", "input:repeat_rate", "25")
        # Measure synchronous compositor dispatch, not completion of child
        # processes that may finish after the repeat key has been released.
        address = self.windows()["hs-B"]["address"]
        self.ctl("dispatch", "setfloating", "address:" + address)
        self.ctl("dispatch", "resizewindowpixel", "exact 400 300,address:" + address)
        self.ctl("keyword", "binde", "CTRL,R,resizewindowpixel,1 0,address:" + address)
        before = self.windows()["hs-B"]["size"][0]
        self.run("wtype", "-M", "ctrl", "-P", "r", "-s", "500", "-p", "r", "-m", "ctrl")
        after = self.windows()["hs-B"]["size"][0]
        assert after >= before + 3
        time.sleep(0.2)
        assert self.windows()["hs-B"]["size"][0] == after
        self.ctl("keyword", "bindr", f"CTRL,Y,exec,touch {shlex.quote(str(release))}")
        key = self.spawn(
            ["wtype", "-M", "ctrl", "-P", "y", "-s", "400", "-p", "y", "-m", "ctrl"]
        )
        time.sleep(0.15)
        assert not release.exists()
        key.wait(timeout=3)
        wait_for(release.exists)
        self.ctl("keyword", "bind", "CTRL,S,submap,hs-test")
        self.ctl("keyword", "submap", "hs-test")
        self.ctl("keyword", "bind", f"CTRL,T,exec,touch {shlex.quote(str(submap))}")
        self.ctl("keyword", "bind", "CTRL,S,submap,reset")
        self.ctl("keyword", "submap", "reset")
        self.run("wtype", "-M", "ctrl", "-k", "s", "-k", "t", "-k", "s", "-m", "ctrl")
        wait_for(submap.exists)
        assert self.status()["live"] and self.status()["modifiers"] == 0
        self.check(
            "native repeat, release and submap bindings run without stuck modifiers"
        )
        self.ctl("keyword", "bind", "CTRL,E,workspace,13")
        self.run("wtype", "-M", "ctrl", "-k", "e", "-m", "ctrl")
        assert (
            self.status()["target"]["workspace"] == 13
            and self.status()["target"]["window"] == "0x0"
        )
        self.run("wtype", "-M", "logo", "-k", "j", "-m", "logo")
        assert not self.data("activewindow")
        self.check(
            "keyboard navigation to an empty workspace never targets another workspace window"
        )
        log = self.root / "input.log"
        log.write_text("")
        self.run("wtype", "-P", "q", "-s", "100", "-p", "q")
        self.close()
        self.ctl(
            "dispatch", "focuswindow", "address:" + self.windows()["hs-A"]["address"]
        )
        self.run("wtype", "z")
        wait_for(lambda: " 122" in log.read_text())
        assert " 113" not in log.read_text()
        self.check(
            "suppressed keys do not leak and ordinary typing resumes after dismissal"
        )

    def first_resize(self):
        self.setup("dwindle", 0, 1)
        address = self.windows()["hs-A"]["address"]
        self.ctl("dispatch", "setfloating", "address:" + address)
        self.ctl("dispatch", "resizewindowpixel", "exact 320 240,address:" + address)
        self.ctl("dispatch", "movewindowpixel", "exact 100 100,address:" + address)
        self.ctl("dispatch", "hyprspace:overview", "on")
        time.sleep(0.15)
        source = self.preview_point("hs-A", 0.7, 0.7)
        self.drag(source, (source[0] + 25, source[1] + 20), True, button=273)
        assert (
            self.windows()["hs-A"]["size"][0] > 340
            and self.windows()["hs-A"]["size"][1] > 260
        )
        self.check(
            "the first native resize survives immediate Super release and flushes its final motion"
        )

    def lifecycle(self):
        self.setup("dwindle", 0, 1)
        self.ctl("dispatch", "hyprspace:overview", "on")
        time.sleep(0.15)
        empty = next(
            tile
            for view in self.status()["views"]
            for tile in view["tiles"]
            if tile["workspace"] == 13 and tile["window"] == "0x0"
        )
        self.drag(
            self.preview_point("hs-A"),
            (empty["x"] + empty["w"] * 0.3, empty["y"] + empty["h"] * 0.4),
            True,
        )
        assert self.windows()["hs-A"]["workspace"]["id"] == 13
        self.check("empty persistent workspaces accept native drops")
        self.move(self.preview_point("hs-B"))
        token = self.request("capture")
        self.ctl("dispatch", "moveworkspacetomonitor", f"12 {self.names[0]}")
        wait_for(
            lambda: any(
                view["monitor"] == self.names[0]
                and any(tile["workspace"] == 12 for tile in view["tiles"])
                for view in self.status()["views"]
            )
        )
        self.spawn(
            [
                str(REPO / "build/hyprspace-launch"),
                "--context",
                token,
                "--",
                "python3",
                str(CLIENT),
                "hs-transfer",
            ]
        )
        wait_for(lambda: "hs-transfer" in self.windows())
        assert self.windows()["hs-transfer"]["workspace"]["id"] == 12
        assert self.windows()["hs-transfer"]["monitor"] == next(
            m["id"] for m in self.data("monitors") if m["name"] == self.names[0]
        )
        assert self.status()["layout_targets_unique"]
        self.check(
            "workspace transfer preserves identity and remaps a frozen launch destination"
        )
        self.move(self.preview_point("hs-transfer"))
        self.run("wtype", "-M", "logo")
        # Keep the modifier alive until the provisional drag has started.
        held = self.spawn(["wtype", "-M", "logo", "-s", "700", "-m", "logo"])
        time.sleep(0.08)
        self.button(1)
        assert self.status()["dragging"]
        os.kill(self.windows()["hs-transfer"]["pid"], signal.SIGTERM)
        wait_for(lambda: not self.status()["dragging"])
        self.button(0)
        held.wait(timeout=3)
        self.check("a disappearing window cancels a stationary provisional drag")
        self.move(self.preview_point("hs-B"))
        held = self.spawn(["wtype", "-M", "logo", "-s", "900", "-m", "logo"])
        time.sleep(0.08)
        self.button(1)
        assert self.status()["dragging"]
        self.ctl("keyword", "monitor", f"{self.names[2]},disable")
        self.await_outputs(2)
        wait_for(lambda: not self.status()["dragging"])
        self.button(0)
        held.wait(timeout=3)
        self.ctl("keyword", "monitor", f"{self.names[2]},960x600@60,2200x100,1.5")
        self.await_outputs(3)
        wait_for(lambda: len(self.status()["views"]) == 3)
        self.close()
        assert all(w["alpha"] == 1 for w in self.status()["windows"])
        self.check(
            "output removal cancels dragging and hotplug restores views without hidden windows"
        )

    def interactions(self):
        self.setup("dwindle", 0, 1)
        self.ctl("dispatch", "hyprspace:overview", "on")
        time.sleep(0.15)
        before = self.geometry()
        self.drag(self.preview_point("hs-A"), (2000, 50), True)
        assert self.geometry() == before
        self.check("drop in a monitor gap cancels without changing layout")
        self.drag(
            self.preview_point("hs-A"), self.preview_point("hs-B"), True, cancel=True
        )
        assert self.geometry() == before
        self.check("Escape cancels a provisional drag and keeps the overview open")
        self.move(self.preview_point("hs-B"))
        assert self.status()["target"]["workspace"] == 12, (
            self.status(),
            self.data("monitors"),
            self.windows(),
        )
        self.run("wtype", "-M", "logo", "-k", "j", "-m", "logo")
        assert self.status()["live"]
        assert self.data("activewindow")["address"] == self.windows()["hs-B"]["address"]
        self.check("Super+J acts on the indicated window")
        self.run("wtype", "-M", "logo", "-k", "l", "-m", "logo")
        assert self.status()["live"]
        assert (
            next(w for w in self.data("workspaces") if w["id"] == 12)["tiledLayout"]
            == "scrolling"
        )
        self.check("Super+L cycles the indicated workspace synchronously")
        marker = self.root / "ctrl"
        self.ctl("keyword", "bind", f"CTRL,U,exec,touch {shlex.quote(str(marker))}")
        self.run("wtype", "-M", "ctrl", "-k", "u", "-m", "ctrl")
        wait_for(marker.exists)
        self.check("Ctrl binding reaches the native matcher")
        log = self.root / "input.log"
        log.write_text("")
        self.run("wtype", "unbound-text")
        assert log.read_text() == ""
        self.check("unbound keys do not leak to application windows")
        self.move(self.preview_point("hs-A"))
        token = self.request("capture")
        assert token
        self.move(self.preview_point("hs-B"))
        self.spawn(
            [
                str(REPO / "build/hyprspace-launch"),
                "--context",
                token,
                "--",
                "sh",
                "-c",
                f"sleep .2; exec python3 {shlex.quote(str(CLIENT))} hs-launch",
            ]
        )
        wait_for(lambda: "hs-launch" in self.windows())
        assert self.windows()["hs-launch"]["workspace"]["id"] == 11
        assert self.status()["live"]
        assert self.request("consume " + token) == ""
        self.check(
            "delayed launch retains activation-time destination and consumes it once"
        )
        self.close()
        assert all(w["alpha"] == 1 for w in self.status()["windows"])
        self.ctl("plugin", "unload", str(PLUGIN))
        assert not (
            self.root
            / "hypr"
            / self.env["HYPRLAND_INSTANCE_SIGNATURE"]
            / "hyprspace.sock"
        ).exists()
        self.check("unload removes private interface and restores window visibility")

    def finish(self):
        try:
            windows = self.windows() if getattr(self, "connected", True) else {}
            for pid in {window["pid"] for window in windows.values()}:
                try:
                    os.kill(pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
        except (subprocess.SubprocessError, OSError, ValueError, AssertionError):
            pass
        for process in reversed(self.processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
        try:
            if self.owned and hasattr(self, "compositor"):
                stop_process_group(self.compositor)
        finally:
            if getattr(self, "background", None):
                self.background.finish()
        (self.root / "results.json").write_text(
            json.dumps({"passed": self.checks}, indent=2)
        )
        print("Artifacts:", self.root, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--visible",
        action="store_true",
        help="open and arrange three test windows on your desktop instead of running in the background",
    )
    parser.add_argument(
        "--runtime", type=Path, help="use an already isolated compositor"
    )
    parser.add_argument("--quick", action="store_true")
    parser.add_argument(
        "--only",
        choices=(
            "all",
            "interactions",
            "matrix",
            "foreground",
            "activation",
            "companions",
            "special",
            "keyboard",
            "lifecycle",
            "resize",
            "browser",
            "discord",
            "audit",
            "scrolling",
        ),
        default="all",
    )
    parser.add_argument(
        "--companions",
        type=Path,
        help="directory containing patched walker, elephant and providers/",
    )
    parser.add_argument(
        "--firefox",
        action="store_true",
        help="include Firefox existing-process checks using a private profile",
    )
    parser.add_argument(
        "--discord",
        type=Path,
        help="include Discord existing-process checks with this binary and a private profile",
    )
    args = parser.parse_args()
    # Let interactive applications win CPU time while tests and their children run.
    os.setpriority(os.PRIO_PROCESS, 0, max(10, os.getpriority(os.PRIO_PROCESS, 0)))

    def interrupted(signum, _frame):
        raise SystemExit(128 + signum)

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGHUP, interrupted)
    print(
        "Mode:",
        "visible desktop windows" if args.visible else "background virtual monitors",
        flush=True,
    )
    for attempt in range(3):
        try:
            suite = Suite(args.runtime, visible=args.visible)
            break
        except OutputUnavailable as error:
            if attempt == 2:
                raise
            print("Retrying unavailable nested output backend:", error, flush=True)
    try:
        if args.only in ("all", "audit", "scrolling"):
            import regressions

            if args.only in ("all", "audit"):
                regressions.audit(suite, wait_for)
            if args.only in ("all", "scrolling"):
                regressions.scrolling(suite, wait_for)
        if args.only in ("all", "resize"):
            suite.first_resize()
        if args.only in ("all", "interactions"):
            suite.interactions()
            suite.ctl("plugin", "load", str(PLUGIN))
        if args.only in ("all", "keyboard"):
            suite.keyboard()
        if args.only in ("all", "foreground"):
            suite.foreground()
        if args.only in ("all", "matrix"):
            suite.matrix(args.quick)
        if args.only in ("all", "special"):
            suite.special()
        if args.only in ("all", "lifecycle"):
            suite.lifecycle()
        if args.only == "companions" or args.only == "all" and args.companions:
            assert args.companions, "--companions is required for the companion suite"
            suite.companions(args.companions)
        if args.only == "browser" or args.only == "all" and args.firefox:
            suite.browser()
        if args.only == "discord" or args.only == "all" and args.discord:
            assert args.discord, "--discord is required for the Discord suite"
            suite.discord(args.discord)
        if args.only in ("all", "activation"):
            suite.activation()
    except Exception:
        try:
            (suite.root / "failure.json").write_text(
                json.dumps(
                    {
                        "status": suite.status(),
                        "monitors": suite.data("monitors"),
                        "windows": suite.windows(),
                        "clients": suite.data("clients"),
                        "layers": suite.data("layers"),
                        "cursor": suite.data("cursorpos"),
                    },
                    indent=2,
                )
            )
            suite.run(
                "grim",
                "-s",
                "1",
                "-c",
                "-o",
                suite.names[0],
                str(suite.root / "failure.png"),
            )
        except Exception:
            pass
        raise
    finally:
        suite.finish()


if __name__ == "__main__":
    main()
