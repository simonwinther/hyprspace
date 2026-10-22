import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "build/hyprspace-launch"


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
                    self.assertEqual(subprocess.check_output([str(HELPER), "--capture"], text=True), "opaque-token\n")
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
        program = "import os,json; print(json.dumps({key:os.environ.get(key) for key in ['HYPRSPACE_LAUNCH_TOKEN','XDG_ACTIVATION_TOKEN']}))"

        def environment():
            return json.loads(subprocess.check_output([str(HELPER), "--launch-token", "", "--", "python3", "-c", program], text=True))

        with patch.dict(
            os.environ, HYPRSPACE_LAUNCH_TOKEN="old", XDG_ACTIVATION_TOKEN="old"
        ):
            env = environment()
        self.assertIsNone(env["HYPRSPACE_LAUNCH_TOKEN"])
        self.assertIsNone(env["XDG_ACTIVATION_TOKEN"])
        with patch.dict(
            os.environ, HYPRSPACE_LAUNCH_TOKEN="old", XDG_ACTIVATION_TOKEN="native"
        ):
            self.assertEqual(
                environment()["XDG_ACTIVATION_TOKEN"], "native"
            )

    def test_automatic_capture_consumes_one_context_before_exec(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "hypr/instance"
            path.mkdir(parents=True)
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as server:
                server.bind(str(path / "hyprspace.sock"))
                server.listen(2)
                server.settimeout(3)
                received = []

                def serve():
                    for response in (b"opaque-context", b"launch-token"):
                        peer, _ = server.accept()
                        with peer:
                            received.append(peer.recv(256))
                            peer.sendall(response)

                worker = threading.Thread(target=serve)
                worker.start()
                env = dict(os.environ, XDG_RUNTIME_DIR=directory, HYPRLAND_INSTANCE_SIGNATURE="instance")
                program = "import os,json; print(json.dumps([os.getenv('HYPRSPACE_LAUNCH_TOKEN'),os.getenv('XDG_ACTIVATION_TOKEN'),os.getenv('HL_INITIAL_WORKSPACE_TOKEN')]))"
                result = subprocess.check_output([str(HELPER), "--", "python3", "-c", program], env=env, text=True, timeout=3)
                worker.join(timeout=3)
                self.assertFalse(worker.is_alive())
                self.assertEqual(received, [b"capture", b"consume opaque-context"])
                self.assertEqual(json.loads(result), ["launch-token", "launch-token", ""])

    def test_invalid_and_unresponsive_socket_replies_keep_normal_launching(self):
        for response in (b"\xff", b"a" * 65537, None):
            with self.subTest(reply=None if response is None else len(response)), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "hypr/instance"
                path.mkdir(parents=True)
                with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as server:
                    server.bind(str(path / "hyprspace.sock"))
                    server.listen(1)
                    server.settimeout(3)

                    def serve():
                        peer, _ = server.accept()
                        with peer:
                            peer.recv(256)
                            if response is None:
                                time.sleep(0.5)
                            else:
                                peer.sendall(response)

                    worker = threading.Thread(target=serve)
                    worker.start()
                    env = dict(os.environ, XDG_RUNTIME_DIR=directory, HYPRLAND_INSTANCE_SIGNATURE="instance")
                    started = time.monotonic()
                    result = subprocess.check_output([str(HELPER), "--context", "context", "--", "printf", "%s", "native"], env=env, text=True, timeout=2)
                    self.assertEqual(result, "native")
                    self.assertLess(time.monotonic() - started, 0.8)
                    worker.join(timeout=3)
                    self.assertFalse(worker.is_alive())

    def test_invalid_instance_and_overlong_socket_path_do_not_delay_exec(self):
        for runtime, instance in (("/tmp", "../other"), ("/tmp", ".."), ("/tmp", "."), ("/" + "a" * 120, "instance")):
            with self.subTest(instance=instance):
                env = dict(os.environ, XDG_RUNTIME_DIR=runtime, HYPRLAND_INSTANCE_SIGNATURE=instance)
                result = subprocess.check_output([str(HELPER), "--context", "context", "--", "printf", "%s", "native"], env=env, text=True, timeout=2)
                self.assertEqual(result, "native")

    def test_non_utf8_arguments_and_environment_are_preserved(self):
        program = "import os,sys,json; print(json.dumps([os.fsencode(sys.argv[1]).hex(),os.environb[b'RAW_VALUE'].hex()]))"
        env = os.environb.copy()
        env[b"RAW_VALUE"] = b"raw-\xff"
        result = subprocess.check_output([os.fsencode(HELPER), b"--launch-token", b"token", b"--", b"python3", b"-c", program.encode(), b"arg-\xfe"], env=env, text=True)
        self.assertEqual(json.loads(result), [b"arg-\xfe".hex(), b"raw-\xff".hex()])

    def test_payload_replaces_the_helper_process(self):
        process = subprocess.Popen([str(HELPER), "--launch-token", "token", "--", "python3", "-c", "import os;print(os.getpid())"], stdout=subprocess.PIPE, text=True)
        stdout, _ = process.communicate(timeout=3)
        self.assertEqual(process.returncode, 0)
        self.assertEqual(int(stdout), process.pid)

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
