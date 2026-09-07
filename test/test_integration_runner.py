#!/usr/bin/env python3
"""Check compositor isolation and cleanup without touching a running desktop."""

import contextlib
import io
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent / "integration"))
from background import BackgroundDisplay, stop_process_group
from run import Suite, validate_runtime


class IsolationTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="hs-runner-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.host = {
            "XDG_RUNTIME_DIR": str(self.root / "desktop"),
            "WAYLAND_DISPLAY": "desktop-wayland",
            "WAYLAND_SOCKET": "9",
            "HYPRLAND_INSTANCE_SIGNATURE": "desktop-instance",
            "DBUS_SESSION_BUS_ADDRESS": "unix:path=/desktop-bus",
            "DISPLAY": ":99",
        }
        self.saved = {
            "XDG_RUNTIME_DIR": str(self.root),
            "WAYLAND_DISPLAY": "wayland-test",
            "HYPRLAND_INSTANCE_SIGNATURE": "test-instance",
            "DBUS_SESSION_BUS_ADDRESS": "unix:path=/private-test-bus",
        }
        self.metadata = {
            "mode": "headless",
            "outputs": [f"WAYLAND-{i+1}" for i in range(3)],
        }
        for path in (
            self.root / "wayland-test",
            self.root / "hypr/test-instance/.socket.sock",
        ):
            path.parent.mkdir(parents=True, exist_ok=True)
            sock = socket.socket(socket.AF_UNIX)
            sock.bind(str(path))
            self.addCleanup(sock.close)

    def test_private_runtime_can_be_reused(self):
        with patch.dict(os.environ, self.host):
            validate_runtime(self.root, self.saved, self.metadata, False)

    def test_packaged_library_requires_fresh_session_and_existing_file(self):
        with self.assertRaisesRegex(ValueError, "existing library"):
            Suite(plugin=self.root / "missing.so")
        library = self.root / "libhyprspace.so"
        library.touch()
        with self.assertRaisesRegex(ValueError, "fresh private session"):
            Suite(runtime=self.root, plugin=library)

    def test_packaged_library_path_survives_startup(self):
        library = self.root / "libhyprspace.so"
        library.touch()

        def fail(suite):
            self.assertEqual(suite.plugin, library.resolve())
            self.assertFalse(suite.visible)
            raise RuntimeError("checked packaged path")

        with patch.object(Suite, "start", fail), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "checked packaged path"):
                Suite(plugin=library)

    def test_reusing_desktop_connections_is_rejected(self):
        for key in (
            "XDG_RUNTIME_DIR",
            "HYPRLAND_INSTANCE_SIGNATURE",
            "DBUS_SESSION_BUS_ADDRESS",
        ):
            with self.subTest(key=key), patch.dict(os.environ, self.host):
                saved = self.saved | {key: self.host[key]}
                with self.assertRaises(ValueError):
                    validate_runtime(self.root, saved, self.metadata, False)

    def test_socket_outside_private_directory_is_rejected(self):
        saved = self.saved | {"WAYLAND_DISPLAY": "/outside/wayland-0"}
        with self.assertRaises(ValueError):
            validate_runtime(self.root, saved, self.metadata, False)

    def test_visible_runtime_requires_explicit_selection(self):
        visible = self.metadata | {"mode": "visible"}
        with self.assertRaises(ValueError):
            validate_runtime(self.root, self.saved, visible, False)
        with patch.dict(os.environ, self.host):
            validate_runtime(self.root, self.saved, visible, True)

    def test_saved_environment_cannot_add_launch_instructions(self):
        saved = self.saved | {"LD_PRELOAD": "/unexpected.so"}
        with self.assertRaises(ValueError):
            validate_runtime(self.root, saved, self.metadata, False)

    def test_startup_failure_never_queries_the_desktop_for_cleanup(self):
        runtime = self.root / "failed-start"
        runtime.mkdir()

        def fail(suite):
            self.assertFalse(suite.visible)
            self.assertEqual(suite.env["XDG_RUNTIME_DIR"], str(runtime))
            self.assertNotIn("WAYLAND_DISPLAY", suite.env)
            self.assertNotIn("WAYLAND_SOCKET", suite.env)
            self.assertNotIn("HYPRLAND_INSTANCE_SIGNATURE", suite.env)
            self.assertNotIn("DISPLAY", suite.env)
            self.assertNotIn("DBUS_SESSION_BUS_ADDRESS", suite.env)
            self.assertEqual(suite.env["LIBSEAT_BACKEND"], "seatd")
            self.assertEqual(suite.env["SEATD_SOCK"], str(runtime / "no-seatd.sock"))
            raise RuntimeError("fixture startup failure")

        with (
            patch.dict(os.environ, self.host),
            patch("run.tempfile.mkdtemp", return_value=str(runtime)),
            patch.object(Suite, "start", fail),
            patch.object(Suite, "windows") as windows,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            with self.assertRaisesRegex(RuntimeError, "fixture startup failure"):
                Suite()
            windows.assert_not_called()

    def test_missing_host_does_not_launch_a_fallback(self):
        display = BackgroundDisplay(self.root, self.host)
        with (
            patch("background.Path.is_file", return_value=False),
            patch("background.subprocess.Popen") as spawn,
        ):
            with self.assertRaisesRegex(RuntimeError, "No desktop session was started"):
                display.start()
            spawn.assert_not_called()

    def test_crashed_host_does_not_launch_a_fallback(self):
        display = BackgroundDisplay(self.root, self.host)
        with (
            patch("background.Path.is_file", return_value=True),
            patch("background.subprocess.Popen") as spawn,
        ):
            spawn.return_value.poll.return_value = 1
            with self.assertRaisesRegex(RuntimeError, "will not fall back"):
                display.start()
            self.assertEqual(spawn.call_count, 1)
            env = spawn.call_args.kwargs["env"]
            self.assertEqual(env["XDG_RUNTIME_DIR"], str(self.root / "parent"))
            for key in self.host.keys() - {"XDG_RUNTIME_DIR"}:
                self.assertNotIn(key, env)

    def test_visible_window_arrangement_is_guarded(self):
        suite = Suite.__new__(Suite)
        suite.visible = False
        with patch("run.subprocess.check_output") as ctl:
            with self.assertRaisesRegex(AssertionError, "require --visible"):
                suite.start_visible_outputs()
            ctl.assert_not_called()

    def test_cleanup_stops_children_after_the_parent_exits(self):
        child_pid = self.root / "child.pid"
        boot = (
            "import os,signal,sys; from pathlib import Path; "
            "pid=os.fork(); "
            "Path(sys.argv[1]).write_text(str(pid)) if pid else None; "
            "os._exit(0) if pid else signal.pause()"
        )
        process = subprocess.Popen(
            [sys.executable, "-c", boot, str(child_pid)], start_new_session=True
        )
        self.addCleanup(stop_process_group, process)
        process.wait(timeout=3)
        pid = int(child_pid.read_text())
        stop_process_group(process)
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            try:
                state = (
                    Path(f"/proc/{pid}/stat").read_text().split(")", 1)[1].split()[0]
                )
                if state == "Z":
                    return
            except FileNotFoundError:
                return
            time.sleep(0.02)
        self.fail("child remains running after cleanup")


if __name__ == "__main__":
    unittest.main()
