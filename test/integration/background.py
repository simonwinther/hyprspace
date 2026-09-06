"""A private headless parent for Hyprland's renderer, with no desktop socket."""

import os
from pathlib import Path
import signal
import subprocess
import time


def stop_process_group(process):
    """Stop our whole session, including descendants of an exited leader."""
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait(timeout=3)


class BackgroundDisplay:
    def __init__(self, root, env):
        self.root = root / "parent"
        self.root.mkdir(mode=0o700)
        self.env = env.copy()
        for name in (
            "DISPLAY",
            "WAYLAND_DISPLAY",
            "WAYLAND_SOCKET",
            "HYPRLAND_INSTANCE_SIGNATURE",
            "DBUS_SESSION_BUS_ADDRESS",
        ):
            self.env.pop(name, None)
        self.env["XDG_RUNTIME_DIR"] = str(self.root)
        self.process = None

    def start(self):
        host = Path(__file__).resolve().parents[2] / "build/test-headless"
        if not host.is_file():
            raise RuntimeError(
                "Build the private display host with make integration-fixtures "
                "(requires wlroots-0.20). No desktop session was started."
            )
        display = self.root / "background"
        with (self.root / "headless.log").open("w") as log:
            self.process = subprocess.Popen(
                [str(host)],
                env=self.env,
                stdout=log,
                stderr=log,
                start_new_session=True,
            )
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and self.process.poll() is None:
            if display.is_socket():
                return str(display)
            time.sleep(0.05)
        raise RuntimeError(
            f"Headless display host did not start; see {self.root / 'headless.log'}. "
            "The runner will not fall back to your desktop."
        )

    def finish(self):
        if self.process:
            stop_process_group(self.process)
            self.process = None
