#!/usr/bin/env python3
"""Exercise staged companion checks and rollback entirely inside temporary homes."""

import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("installer", ROOT / "scripts/install-companions.py")
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class CompanionInstallTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="hs-companion-install-")
        self.addCleanup(temporary.cleanup)
        self.home = Path(temporary.name)
        self.source = self.home / "build"
        self.source.mkdir()
        self.providers = self.home / "installed-providers"
        self.providers.mkdir()
        record = json.loads((ROOT / "companion/versions.json").read_text())
        for name, metadata in record.items():
            metadata["patch_sha256"] = installer.digest(ROOT / f'companion/{name}-{metadata["version"]}.patch')
        record["binaries"] = {}
        for name in ("walker", "elephant", "providers/desktopapplications.so", "providers/menus.so"):
            path = self.source / name
            path.parent.mkdir(exist_ok=True)
            path.write_bytes(b"verified " + name.encode())
            path.chmod(0o755)
            record["binaries"][name] = installer.digest(path)
        (self.providers / "menus.so").touch()
        (self.source / "compatibility.json").write_text(json.dumps(record))
        self.helper = ROOT / "contrib/hyprspace-launch"
        self.data = self.home / "data"
        self.bin = self.home / "bin"
        self.config = self.home / "config"
        self.bin.mkdir()

    def stage(self):
        return installer.stage(self.source, self.helper, self.data, self.providers)

    def test_stage_is_verified_immutable_and_does_not_write_launchers(self):
        release = self.stage()
        self.assertEqual(installer.digest(release / "bin/walker"), installer.digest(self.source / "walker"))
        self.assertTrue((release / "launch-assets/launch-bin/uwsm").is_symlink())
        self.assertFalse((self.bin / "walker").exists())
        self.assertEqual(self.stage(), release)
        (release / "bin/walker").write_bytes(b"corrupted")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            self.stage()

    def test_source_tampering_and_missing_installed_provider_are_rejected(self):
        (self.providers / "clipboard.so").touch()
        with self.assertRaisesRegex(ValueError, "all installed providers"):
            self.stage()
        (self.providers / "clipboard.so").unlink()
        (self.source / "elephant").write_bytes(b"different")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            self.stage()

    def test_service_dropin_preserves_original_flags_and_last_assignment(self):
        unit = '[Service]\nExecStart=/usr/bin/elephant --old\n[Service]\nExecStart=\nExecStart=-/usr/bin/elephant --config "/config with spaces" --debug\n'
        override = installer.service_override(Path('/home/user name/bin/elephant'), unit).decode()
        self.assertIn('ExecStart=-"/home/user name/bin/elephant" --config "/config with spaces" --debug', override)
        with self.assertRaisesRegex(ValueError, "directly execute"):
            installer.service_override(Path('/bin/elephant'), '[Service]\nExecStart=/usr/bin/custom-daemon\n')

    def test_wrapper_preserves_argv_and_existing_background_menu_route(self):
        release = self.stage()
        delegate = self.home / "delegate"
        delegate.write_text('#!/bin/sh\nif [ "$1" = menus:omarchyBackgroundSelector ]; then printf background-grid; else printf "%s\\n" "$@"; fi\n')
        delegate.chmod(0o755)
        launcher = self.bin / "omarchy-launch-walker"
        launcher.write_bytes(installer.wrapper(release, delegate))
        launcher.chmod(0o755)
        self.assertEqual(subprocess.check_output([str(launcher), "menus:omarchyBackgroundSelector"], text=True), "background-grid")
        self.assertEqual(subprocess.check_output([str(launcher), "--width", "644", "value with spaces"], text=True), "--width\n644\nvalue with spaces\n")

    def test_activation_and_rollback_restore_regular_files_and_symlinks(self):
        release = self.stage()
        launcher = self.bin / "omarchy-launch-walker"
        launcher.write_text('#!/bin/sh\nexec /original/launcher "$@"\n')
        launcher.chmod(0o755)
        original = launcher.read_bytes()
        (self.bin / "walker").symlink_to("/usr/bin/walker")
        (self.bin / "elephant").write_text("original elephant override")
        with (patch.object(installer, "walker_services", return_value=[]),
              patch.object(installer.subprocess, "check_output", return_value="[Service]\nExecStart=elephant --debug\n"),
              patch.object(installer.subprocess, "run", return_value=SimpleNamespace(returncode=0)),
              patch.object(installer, "start_walkers")):
            manifest = installer.activate(release, self.data, self.bin, self.config)
            self.assertEqual(installer.activate(release, self.data, self.bin, self.config), manifest)
            self.assertIn(b"original-omarchy-launch-walker", launcher.read_bytes())
            dropin = self.config / "systemd/user/elephant.service.d/zz-hyprspace-companions.conf"
            self.assertIn(b"--debug", dropin.read_bytes())
            installer.rollback(manifest)
            self.assertFalse(dropin.exists())
            self.assertEqual(launcher.read_bytes(), original)
            self.assertTrue((self.bin / "walker").is_symlink())
            self.assertEqual((self.bin / "elephant").read_text(), "original elephant override")
            self.assertFalse((self.data / "active.json").exists())

    def test_rollback_protects_later_user_edits(self):
        release = self.stage()
        with (patch.object(installer, "walker_services", return_value=[]),
              patch.object(installer.subprocess, "check_output", return_value="[Service]\nExecStart=elephant\n"),
              patch.object(installer.subprocess, "run", return_value=SimpleNamespace(returncode=0)),
              patch.object(installer, "start_walkers")):
            manifest = installer.activate(release, self.data, self.bin, self.config)
            (self.bin / "walker").write_text("later user edit")
            with self.assertRaisesRegex(ValueError, "changed after installation"):
                installer.rollback(manifest)
            self.assertEqual((self.bin / "walker").read_text(), "later user edit")


if __name__ == "__main__":
    unittest.main()
