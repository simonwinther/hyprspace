import json
import os
from pathlib import Path
import runpy
import socket
import subprocess
import tempfile
import threading
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "build/hyprspace-launch"
helper = runpy.run_path(str(ROOT / "contrib/hyprspace-launch"))


class LaunchHelperTests(unittest.TestCase):
    def test_argv_and_process_environment(self):
        program = "import os,sys,json; print(json.dumps([sys.argv[1:],os.getenv('HYPRSPACE_LAUNCH_TOKEN')]))"
        args = ["hello world", "$(touch must-not-exist)", "quote'\"", "a\nb", "--"]
        result = subprocess.check_output(
            [
                str(HELPER),
                "--launch-token",
                "test-token",
                "--",
                "python3",
                "-c",
                program,
                *args,
            ],
            text=True,
        )
        self.assertEqual(json.loads(result), [args, "test-token"])
        self.assertNotEqual(os.environ.get("HYPRSPACE_LAUNCH_TOKEN"), "test-token")

    def test_service_forwarding_preserves_payload_and_options(self):
        with tempfile.TemporaryDirectory() as directory:
            original = Path(directory) / "uwsm-app"
            original.write_text(
                "#!/usr/bin/python3\nimport sys,json\nprint(json.dumps(sys.argv[1:]))\n"
            )
            original.chmod(0o755)
            env = dict(os.environ, PATH=f"{directory}:/usr/bin")
            command = [
                str(HELPER),
                "--launch-token",
                "service-token",
                "--",
                "uwsm-app",
                "-s",
                "a",
                "--",
                "app",
                "argument with spaces",
            ]
            result = json.loads(subprocess.check_output(command, env=env, text=True))
            self.assertEqual(result[:3], ["-s", "a", "--"])
            self.assertEqual(
                result[4:],
                [
                    "--launch-token",
                    "service-token",
                    "--",
                    "app",
                    "argument with spaces",
                ],
            )

    def test_service_payload_separators_preserve_native_arguments_and_context(self):
        program = (
            "import os,sys,json; "
            "print(json.dumps([sys.argv[1:],os.getenv('HYPRSPACE_LAUNCH_TOKEN'),"
            "os.getenv('XDG_ACTIVATION_TOKEN')]))"
        )
        payloads = [
            ["plain argument"],
            ["hello world", "--", "after separator", "--"],
            ["--", "org.example.App.desktop:NewWindow", "url with spaces"],
            ["--"],
        ]
        with tempfile.TemporaryDirectory() as directory:
            env = dict(os.environ, PATH=f"{directory}:/usr/bin")
            application = Path(directory) / "dump.py"
            application.write_text(program)
            launchers = {
                "app2unit": [[], ["--"]],
                "uwsm-app": [[], ["--"], ["-s", "a", "--"]],
                "uwsm": [["app"], ["app", "--"], ["app", "-s", "a", "--"]],
            }
            for name, prefixes in launchers.items():
                original = Path(directory) / name
                original.write_text(
                    "#!/usr/bin/python3\n"
                    "import argparse,os,sys\n"
                    "name = os.path.basename(sys.argv[0])\n"
                    "args = sys.argv[1:]\n"
                    "if name == 'uwsm':\n"
                    "    assert args.pop(0) == 'app'\n"
                    "if name == 'app2unit':\n"
                    "    if args[0] == '--':\n"
                    "        args.pop(0)\n"
                    "else:\n"
                    "    parser = argparse.ArgumentParser()\n"
                    "    parser.add_argument('-s')\n"
                    "    parser.add_argument('cmdline', nargs='+')\n"
                    "    args = parser.parse_args(args).cmdline\n"
                    "env = os.environ.copy()\n"
                    "env.pop('HYPRSPACE_LAUNCH_TOKEN', None)\n"
                    "env.pop('XDG_ACTIVATION_TOKEN', None)\n"
                    "os.execvpe(args[0], args, env)\n"
                )
                original.chmod(0o755)
                for prefix in prefixes:
                    for args in payloads:
                        with self.subTest(launcher=name, prefix=prefix, args=args):
                            expected_args = args.copy()
                            token = "service-token"
                            if name != "app2unit" and "--" not in prefix:
                                # UWSM consumes its first separator even after
                                # the command. Such forms keep native routing.
                                if "--" in expected_args:
                                    expected_args.remove("--")
                                token = None
                            result = subprocess.check_output(
                                [
                                    str(HELPER),
                                    "--launch-token",
                                    "service-token",
                                    "--",
                                    name,
                                    *prefix,
                                    "python3",
                                    str(application),
                                    *args,
                                ],
                                env=env,
                                text=True,
                                timeout=5,
                            )
                            self.assertEqual(json.loads(result), [expected_args, token, token])

    def test_private_instance_socket_and_one_packet(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "hypr/instance"
            path.mkdir(parents=True)
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as server:
                server.bind(str(path / "hyprspace.sock"))
                server.listen(1)
                received = []

                def serve():
                    peer, _ = server.accept()
                    with peer:
                        received.append(peer.recv(256))
                        peer.sendall(b"opaque-token")

                worker = threading.Thread(target=serve)
                worker.start()
                with patch.dict(
                    os.environ,
                    XDG_RUNTIME_DIR=directory,
                    HYPRLAND_INSTANCE_SIGNATURE="instance",
                ):
                    self.assertEqual(helper["request"]("capture"), "opaque-token")
                worker.join(timeout=2)
                self.assertFalse(worker.is_alive())
                self.assertEqual(received, [b"capture"])

    def test_missing_socket_keeps_native_launch(self):
        with patch.dict(
            os.environ,
            XDG_RUNTIME_DIR="/nonexistent",
            HYPRLAND_INSTANCE_SIGNATURE="missing",
        ):
            result = subprocess.check_output(
                [
                    str(HELPER),
                    "--context",
                    "expired",
                    "--",
                    "printf",
                    "%s",
                    "unchanged",
                ],
                text=True,
            )
        self.assertEqual(result, "unchanged")

    def test_expired_context_does_not_inherit_a_previous_destination(self):
        with patch.dict(
            os.environ, HYPRSPACE_LAUNCH_TOKEN="old", XDG_ACTIVATION_TOKEN="old"
        ):
            env = helper["launch_environment"]("")
        self.assertNotIn("HYPRSPACE_LAUNCH_TOKEN", env)
        self.assertNotIn("XDG_ACTIVATION_TOKEN", env)
        with patch.dict(
            os.environ, HYPRSPACE_LAUNCH_TOKEN="old", XDG_ACTIVATION_TOKEN="native"
        ):
            self.assertEqual(
                helper["launch_environment"]("")["XDG_ACTIVATION_TOKEN"], "native"
            )

    def test_service_desktop_entries_keep_native_argument_parsing(self):
        with tempfile.TemporaryDirectory() as directory:
            original = Path(directory) / "app2unit"
            original.write_text(
                "#!/usr/bin/python3\nimport sys,json\nprint(json.dumps(sys.argv[1:]))\n"
            )
            original.chmod(0o755)
            env = dict(os.environ, PATH=f"{directory}:/usr/bin")
            args = ["--", "org.example.App.desktop:NewWindow", "url with spaces"]
            result = subprocess.check_output(
                [
                    str(HELPER),
                    "--launch-token",
                    "service-token",
                    "--",
                    "app2unit",
                    *args,
                ],
                env=env,
                text=True,
            )
            self.assertEqual(json.loads(result), args)

    def test_service_search_skips_duplicate_shim_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            shim = root / "shim"
            shim.mkdir()
            (shim / "app2unit").symlink_to(HELPER)
            original = root / "app2unit"
            original.write_text(
                "#!/usr/bin/python3\nimport sys,json\nprint(json.dumps(sys.argv[1:]))\n"
            )
            original.chmod(0o755)
            env = dict(os.environ, PATH=f"{shim}:{shim}/../shim:{root}:/usr/bin")
            args = ["--", "org.example.App.desktop"]
            result = subprocess.check_output(
                [
                    str(HELPER),
                    "--launch-token",
                    "service-token",
                    "--",
                    "app2unit",
                    *args,
                ],
                env=env,
                text=True,
                timeout=3,
            )
            self.assertEqual(json.loads(result), args)


if __name__ == "__main__":
    unittest.main()
