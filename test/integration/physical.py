#!/usr/bin/env python3
"""Opt-in physical-output checks with temporary workspaces and state restoration."""

import argparse
import itertools
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time

from run import CLIENT, REPO, Suite, wait_for


class Physical(Suite):
    def __init__(self):
        self.root = Path(tempfile.mkdtemp(prefix="hs-physical."))
        self.root.chmod(0o700)
        self.env = os.environ.copy()
        self.processes = []
        self.checks = []
        self.owned = False
        self.env["HS_INPUT_LOG"] = str(self.root / "input.log")
        for key in (
            "HYPRSPACE_LAUNCH_TOKEN",
            "XDG_ACTIVATION_TOKEN",
            "HL_INITIAL_WORKSPACE_TOKEN",
        ):
            self.env.pop(key, None)
        self.original_monitors = self.data("monitors")
        assert (
            len(self.original_monitors) == 3
        ), "physical suite requires three enabled outputs"
        assert all(not m["name"].startswith("WAYLAND-") for m in self.original_monitors)
        assert not self.windows(), "another integration fixture is already running"
        self.original_windows = {
            w["address"]: w["workspace"]["id"] for w in self.data("clients")
        }
        self.original_focus = self.data("activewindow").get("address")
        self.original_cursor = self.data("cursorpos")
        self.original_options = {
            name: json.loads(self.ctl("-j", "getoption", name))["int"]
            for name in (
                "animations:enabled",
                "cursor:no_hardware_cursors",
                "input:resolve_binds_by_sym",
            )
        }
        ids = {w["id"] for w in self.data("workspaces")}
        self.base = next(
            base
            for base in range(90001, 100000, 3)
            if not ids.intersection(range(base, base + 3))
        )
        self.names = [m["name"] for m in self.original_monitors]
        # The diagnostic interface belongs to the already-loaded plugin.
        assert (
            "layout_targets_unique" in self.status()
        ), "load the matching interactive plugin first"
        self.pointer = self.spawn(
            [str(REPO / "build/test-pointer")],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
        )
        assert self.pointer.stdout.readline().strip() == "ready"
        self.ctl("keyword", "animations:enabled", "false")

    def request(self, message):
        import socket

        path = (
            Path(self.env["XDG_RUNTIME_DIR"])
            / "hypr"
            / self.env["HYPRLAND_INSTANCE_SIGNATURE"]
            / "hyprspace.sock"
        )
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as connection:
            connection.settimeout(2)
            connection.connect(str(path))
            connection.sendall(message.encode())
            return connection.recv(65536).decode()

    def setup(self, layout, source, destination):
        self.close()
        for process in self.processes[1:]:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
        self.processes = self.processes[:1]
        wait_for(lambda: not self.windows())
        for i, name in enumerate(self.names):
            self.ctl(
                "keyword", "workspace", f"{self.base+i},monitor:{name},layout:{layout}"
            )
            self.ctl("dispatch", "focusmonitor", name)
            self.ctl("dispatch", "workspace", str(self.base + i))
        self.ctl("dispatch", "focusmonitor", self.names[destination])
        monitor = next(
            m for m in self.data("monitors") if m["name"] == self.names[destination]
        )
        point = (
            monitor["x"] + monitor["reserved"][0] + 30,
            monitor["y"] + monitor["reserved"][1] + 30,
        )
        self.move(point)
        # Native dwindle insertion depends on pointer position and map order.
        # Seed both before comparing a desktop gesture with its overview peer.
        for count, title in enumerate(("hs-A", "hs-B", "hs-C"), 1):
            self.spawn(["python3", str(CLIENT), title])
            wait_for(lambda: len(self.windows()) == count)
            self.ctl(
                "dispatch", "focuswindow", "address:" + self.windows()[title]["address"]
            )
            self.move(self.point(self.windows()[title], 0.5, 0.5))
        for title, index in (
            ("hs-A", source),
            ("hs-B", destination),
            ("hs-C", destination),
        ):
            self.ctl(
                "dispatch",
                "movetoworkspacesilent",
                f'{self.base+index},address:{self.windows()[title]["address"]}',
            )
        self.ctl(
            "dispatch", "focuswindow", "address:" + self.windows()["hs-A"]["address"]
        )
        time.sleep(0.15)

    def matrix(self):
        for layout in ("dwindle", "scrolling", "master"):
            for source, destination in itertools.product(range(3), repeat=2):
                self.setup(layout, source, destination)
                initial = self.geometry()
                pickup = self.point(self.windows()["hs-A"])
                drop = self.point(self.windows()["hs-B"], 0.25, 0.45)
                for title, point in (("hs-A", pickup), ("hs-B", drop)):
                    window = self.windows()[title]
                    monitor = next(
                        m for m in self.data("monitors") if m["id"] == window["monitor"]
                    )
                    width, height = monitor["width"], monitor["height"]
                    if monitor["transform"] % 2:
                        width, height = height, width
                    assert (
                        monitor["x"]
                        <= point[0]
                        < monitor["x"] + width / monitor["scale"]
                    )
                    assert (
                        monitor["y"]
                        <= point[1]
                        < monitor["y"] + height / monitor["scale"]
                    )
                self.drag(
                    pickup,
                    drop,
                    False,
                )
                expected = self.geometry()
                assert expected["hs-A"][0] == self.base + destination
                self.setup(layout, source, destination)
                assert self.geometry() == initial, (
                    "different starting layouts",
                    layout,
                    source,
                    destination,
                    initial,
                    self.geometry(),
                )
                self.ctl("dispatch", "hyprspace:overview", "on")
                time.sleep(0.2)
                self.drag(
                    self.preview_point("hs-A"),
                    self.preview_point("hs-B", 0.25, 0.45),
                    True,
                )
                actual = self.geometry()
                assert actual == expected, (
                    layout,
                    source,
                    destination,
                    expected,
                    actual,
                )
                self.check(
                    f"physical {layout}: native-equivalent {self.names[source]} -> {self.names[destination]}"
                )

    def cursors(self):
        from PIL import Image, ImageChops

        self.setup("dwindle", 0, 1)
        self.ctl(
            "dispatch",
            "movetoworkspacesilent",
            f'{self.base+2},address:{self.windows()["hs-C"]["address"]}',
        )
        self.ctl("dispatch", "hyprspace:overview", "on")
        time.sleep(0.2)
        for software in (0, 1):
            self.ctl("keyword", "cursor:no_hardware_cursors", str(software))
            for title in ("hs-A", "hs-B", "hs-C"):
                window = self.windows()[title]
                monitor = next(
                    m for m in self.data("monitors") if m["id"] == window["monitor"]
                )
                point = self.preview_point(title)
                self.move(point)
                time.sleep(0.15)
                monitor = next(
                    m for m in self.data("monitors") if m["id"] == window["monitor"]
                )
                assert monitor["hardwareCursorsInUse"] == (software == 0)
                first = self.root / f'cursor-{software}-{monitor["name"]}-first.png'
                second = self.root / f'cursor-{software}-{monitor["name"]}-second.png'
                # Preview geometry can settle after the warp. Crop around the
                # coordinate we actually sent, not a recomputed tile point.
                self.run("grim", "-s", "1", "-c", "-o", monitor["name"], str(first))
                self.move(self.preview_point(title, 0.7, 0.4))
                self.run("grim", "-s", "1", "-c", "-o", monitor["name"], str(second))
                x, y = point[0] - monitor["x"], point[1] - monitor["y"]
                with Image.open(first) as a, Image.open(second) as b:
                    crop = (int(x) - 8, int(y) - 8, int(x) + 64, int(y) + 64)
                    assert ImageChops.difference(
                        a.convert("RGB").crop(crop), b.convert("RGB").crop(crop)
                    ).getbbox()
                self.check(
                    f'physical {monitor["name"]}: visible {"software" if software else "hardware"} cursor'
                )
        assert self.layer("waybar")
        for monitor in self.data("monitors"):
            self.run(
                "grim",
                "-s",
                "1",
                "-c",
                "-o",
                monitor["name"],
                str(self.root / f'overview-{monitor["name"]}.png'),
            )

    def scrolling(self):
        from regressions import arrow, center, tile

        for index, name in enumerate(self.names):
            self.setup("scrolling", index, index)
            self.ctl(
                "keyword", "workspace", f"{self.base+index},layoutopt:direction:right"
            )
            first = min(
                self.windows(), key=lambda title: self.windows()[title]["at"][0]
            )
            self.ctl(
                "dispatch", "focuswindow", "address:" + self.windows()[first]["address"]
            )
            self.ctl("dispatch", "hyprspace:overview", "on")
            time.sleep(0.25)
            box = tile(self, self.base + index)
            self.move(center(box))
            before = self.windows()[first]["at"][0]
            self.scroll()
            wait_for(lambda: self.windows()[first]["at"][0] < before)
            before = self.windows()[first]["at"][0]
            self.scroll(delta=-8, discrete=0, axis=1, source=1)
            wait_for(lambda: self.windows()[first]["at"][0] > before)
            self.scroll(delta=0, discrete=0, axis=1, source=1)
            for _ in range(12):
                self.scroll(delta=-15, discrete=-1)
            self.move(arrow(box, True, True))
            self.button(1)
            self.button(0)
            assert self.status()["live"]
            selected = self.status()["target"]["window"]
            assert selected != "0x0"
            self.run("wtype", "-k", "Page_Up", "-k", "Page_Down")
            assert self.status()["target"]["window"] == selected
            self.run("wtype", "-k", "Return")
            wait_for(lambda: not self.status()["live"])
            assert self.data("activewindow")["address"] == selected
            self.check(
                f"physical {name}: wheel, touchpad, edge arrow and keyboard scrolling"
            )

    def resizes(self):
        for index, name in enumerate(self.names):
            self.setup("dwindle", index, index)
            monitor = next(m for m in self.data("monitors") if m["name"] == name)
            address = self.windows()["hs-A"]["address"]
            self.ctl("dispatch", "setfloating", "address:" + address)
            self.ctl(
                "dispatch", "resizewindowpixel", "exact 320 240,address:" + address
            )
            self.ctl(
                "dispatch",
                "movewindowpixel",
                f'exact {monitor["x"]+100} {monitor["y"]+100},address:{address}',
            )
            self.ctl("dispatch", "hyprspace:overview", "on")
            time.sleep(0.2)
            pickup = self.preview_point("hs-A", 0.7, 0.7)
            self.drag(pickup, (pickup[0] + 25, pickup[1] + 20), True, button=273)
            wait_for(
                lambda: self.windows()["hs-A"]["size"][0] > 340
                and self.windows()["hs-A"]["size"][1] > 260
            )
            assert self.status()["modifiers"] == 0
            self.check(
                f"physical {name}: committed resize survives immediate Super release"
            )

    def launcher(self):
        self.setup("dwindle", 0, 1)
        name = f"hyprspace-verification-{os.getpid()}"
        applications = (
            Path(self.env.get("XDG_DATA_HOME", str(Path.home() / ".local/share")))
            / "applications"
        )
        applications.mkdir(parents=True, exist_ok=True)
        entry = applications / f"{name}.desktop"
        assert not entry.exists()
        entry.write_text(
            f'[Desktop Entry]\nType=Application\nName={name}\nExec=/usr/bin/python3 "{CLIENT}" hs-live-launch\nTerminal=false\nStartupNotify=true\n'
        )
        try:
            time.sleep(0.4)
            self.ctl("dispatch", "hyprspace:overview", "on")
            time.sleep(0.2)
            self.move(self.preview_point("hs-A"))
            self.run("walker", "-m", "desktopapplications")
            wait_for(lambda: self.layer("walker"))
            wait_for(lambda: not self.status()["keyboard_owned"])
            self.run("wtype", name)
            time.sleep(0.5)
            self.move(self.preview_point("hs-B"))
            assert self.status()["target"]["workspace"] == self.base + 1
            self.run("wtype", "-k", "Return")
            self.move(self.preview_point("hs-A", 0.7, 0.6))
            wait_for(lambda: "hs-live-launch" in self.windows(), timeout=12)
            assert self.windows()["hs-live-launch"]["workspace"]["id"] == self.base + 1
            assert self.status()["live"]
            self.check(
                "installed Walker and Elephant preserve the captured desktop-application destination through the service launcher"
            )
            wait_for(lambda: not self.layer("walker"))
            before = {
                layer["address"]
                for monitor in self.data("layers").values()
                for levels in monitor["levels"].values()
                for layer in levels
            }
            notification = self.run(
                "notify-send",
                "-p",
                "-t",
                "3000",
                "Hyprspace verification",
                "Notification above the workspace overview",
            )
            try:

                def new_layer():
                    return next(
                        (
                            (name, layer)
                            for name, monitor in self.data("layers").items()
                            for levels in monitor["levels"].values()
                            for layer in levels
                            if layer["address"] not in before
                        ),
                        None,
                    )

                name, layer = wait_for(new_layer)
                self.run(
                    "grim",
                    "-s",
                    "1",
                    "-c",
                    "-o",
                    name,
                    str(self.root / "notification.png"),
                )
                (self.root / "notification-layer.json").write_text(
                    json.dumps({"monitor": name, "layer": layer}, indent=2)
                )
                self.check("installed notification daemon maps above the live overview")
            finally:
                self.run(
                    "gdbus",
                    "call",
                    "--session",
                    "--dest",
                    "org.freedesktop.Notifications",
                    "--object-path",
                    "/org/freedesktop/Notifications",
                    "--method",
                    "org.freedesktop.Notifications.CloseNotification",
                    notification,
                )
            assert (
                subprocess.run(
                    ["pgrep", "-x", "slurp"], stdout=subprocess.DEVNULL
                ).returncode
                != 0
            ), "another screenshot selection is running"
            self.ctl("keyword", "input:resolve_binds_by_sym", "true")

            def selectors():
                return [
                    layer
                    for monitor in self.data("layers").values()
                    for levels in monitor["levels"].values()
                    for layer in levels
                    if layer["namespace"] in ("slurp", "selection", "hyprpicker")
                ]

            assert not selectors(), "another screenshot overlay is running"
            try:
                self.run("wtype", "-k", "Print")
                # Layer creation precedes mapping and keyboard focus. Wait for
                # every selector's map animation goal before sending Escape.
                wait_for(
                    lambda: sum(
                        layer["namespace"] in ("slurp", "selection")
                        and layer["alpha"] > 0
                        and layer["w"] > 0
                        and layer["h"] > 0
                        for layer in selectors()
                    )
                    == len(self.names)
                )
                assert self.status()["live"] and not self.status()["keyboard_owned"]
                # slurp 1.5 sets running=true after its startup roundtrips;
                # an Escape during them can be overwritten by initialization.
                time.sleep(0.15)
                self.run("wtype", "-k", "Escape")
                wait_for(lambda: not selectors())
            finally:
                remaining = selectors()
                (self.root / "selector-teardown.json").write_text(
                    json.dumps(remaining, indent=2)
                )
                for pid in {layer["pid"] for layer in remaining}:
                    try:
                        os.kill(pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
            wait_for(lambda: self.status()["keyboard_owned"])
            assert self.status()["live"]
            self.check(
                "installed screenshot freeze and selector receive Print/Escape and restore overview input"
            )
        finally:
            entry.unlink(missing_ok=True)
            self.run("walker", "--close")

    def restore(self):
        try:
            self.close()
            for process in self.processes[1:]:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)
            for monitor in self.original_monitors:
                self.ctl("dispatch", "focusmonitor", monitor["name"])
                self.ctl("dispatch", "workspace", monitor["activeWorkspace"]["name"])
            # Remove temporary workspace rules through native config reload.
            self.ctl("reload")
            time.sleep(0.2)
            for name, value in self.original_options.items():
                self.ctl("keyword", name, str(value))
            if self.original_focus:
                self.ctl("dispatch", "focuswindow", "address:" + self.original_focus)
            self.move((self.original_cursor["x"], self.original_cursor["y"]))
            current = {w["address"]: w["workspace"]["id"] for w in self.data("clients")}
            assert all(
                current.get(address, workspace) == workspace
                for address, workspace in self.original_windows.items()
            )
            assert all(w["alpha"] == 1 for w in self.status()["windows"])
            assert self.ctl("configerrors") == ""
            self.check(
                "physical workspaces, focus, cursor settings and window visibility restored"
            )
        finally:
            self.finish()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--run", action="store_true", help="exercise the current physical desktop"
    )
    parser.add_argument(
        "--launcher",
        action="store_true",
        help="test installed companions, notifications and screenshot selector instead of the layout matrix",
    )
    parser.add_argument(
        "--firefox",
        action="store_true",
        help="test Firefox with a private profile instead of the layout matrix",
    )
    parser.add_argument(
        "--discord",
        type=Path,
        help="test this Discord binary with a private profile instead of the layout matrix",
    )
    args = parser.parse_args()
    if not args.run:
        parser.error(
            "--run is required: this suite temporarily switches physical workspaces"
        )
    if args.launcher and (args.firefox or args.discord):
        parser.error(
            "test live launcher services separately from private application sessions"
        )
    suite = Physical()
    try:
        if args.launcher:
            suite.launcher()
        elif args.firefox or args.discord:
            if args.firefox:
                suite.browser()
            if args.discord:
                suite.discord(args.discord)
        else:
            suite.matrix()
            suite.resizes()
            suite.scrolling()
            suite.cursors()
    except Exception:
        (suite.root / "failure.json").write_text(
            json.dumps(
                {
                    "status": suite.status(),
                    "monitors": suite.data("monitors"),
                    "windows": suite.windows(),
                    "clients": suite.data("clients"),
                },
                indent=2,
            )
        )
        raise
    finally:
        suite.restore()


if __name__ == "__main__":
    main()
