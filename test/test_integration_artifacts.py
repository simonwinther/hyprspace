#!/usr/bin/env python3
"""Generation isolation and bounded fixture protocol tests; no desktop needed."""

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent / "integration"))
from artifacts import BINARIES, FIXTURES, MARKER, Snapshot, digest, inventory, prune_snapshots
from protocol import reply


class ArtifactsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="hs-artifacts-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.build = self.root / "build"
        self.build.mkdir()
        self.manifest = self.build / "integration.json"
        self.publish("A")

    def publish(self, generation):
        paths = {}
        for name in (*BINARIES, "client.py", "layer.py", "lib/private.so"):
            path = self.build / name
            path.parent.mkdir(exist_ok=True)
            path.write_text(generation + ":" + name)
            paths[name] = path
        for name in FIXTURES:
            with paths[name].open("ab") as stream:
                stream.write(MARKER + digest(paths["hyprspace.so"]).encode() + b"\0")
        self.manifest.write_text(json.dumps({"schema": 1, "build": generation, "revision": "test", "files": inventory(paths)}))

    def snapshot(self, name="run", **kwargs):
        root = self.root / name
        root.mkdir()
        return Snapshot.create(root, self.manifest, **kwargs)

    def test_existing_run_keeps_A_and_new_run_uses_B_including_fixtures(self):
        first = self.snapshot("A")
        original = {name: digest(first.path(name)) for name in first.record["files"]}
        self.publish("B")  # Deliberately truncate/replace every repository artifact.
        second = self.snapshot("B")
        for name, expected in original.items():
            self.assertEqual(digest(first.path(name)), expected, name)
            self.assertNotEqual(digest(second.path(name)), expected, name)
            self.assertNotEqual(first.path(name).stat().st_ino, (self.build / name).stat().st_ino)
        self.assertNotEqual(first.record["generation"], second.record["generation"])
        self.assertEqual(Snapshot.attach(first.root.parent).record, first.record)

    def test_stale_inventory_rejects_new_main_plugin(self):
        (self.build / "hyprspace.so").write_text("B")
        with self.assertRaisesRegex(ValueError, "Build changed"):
            self.snapshot()
        self.assertFalse((self.root / "run/artifacts").exists())

    def test_republishing_hashes_cannot_hide_mismatched_fixture(self):
        for name in FIXTURES:
            with self.subTest(name=name):
                self.publish("A")
                old_fixture = (self.build / name).read_bytes()
                self.publish("B")
                (self.build / name).write_bytes(old_fixture)
                record = json.loads(self.manifest.read_text())
                record["files"][name]["sha256"] = digest(self.build / name)
                self.manifest.write_text(json.dumps(record))
                with self.assertRaisesRegex(ValueError, "different plugin"):
                    self.snapshot(name)

    def test_rebuild_during_copy_fails_before_starting_a_run(self):
        copy = shutil.copyfile

        def replace(source, destination):
            if Path(source).name == "test-overview.so":
                self.publish("B")
            return copy(source, destination)

        with patch("artifacts.shutil.copyfile", replace), self.assertRaisesRegex(ValueError, "Build changed"):
            self.snapshot()

    def test_missing_snapshot_file_never_falls_back_to_build(self):
        snapshot = self.snapshot()
        snapshot.path("test-lock").unlink()
        with self.assertRaises(FileNotFoundError):
            snapshot.path("test-lock")
        with self.assertRaises(FileNotFoundError):
            Snapshot.attach(snapshot.root.parent)

    def test_incomplete_inventory_is_rejected_before_copying(self):
        record = json.loads(self.manifest.read_text())
        del record["files"]["test-lock"]
        self.manifest.write_text(json.dumps(record))
        with self.assertRaisesRegex(ValueError, "Incomplete integration inventory"):
            self.snapshot()

    def test_completed_cleanup_retains_manifest_and_failure_diagnostics(self):
        good = self.snapshot("good")
        good.finish(False)
        self.assertFalse(good.root.exists())
        self.assertTrue((good.root.parent / "generation.json").exists())
        failed = self.snapshot("hs-i.failed")
        failed.finish(True)
        prune_snapshots(self.root)
        self.assertTrue(failed.path("hyprspace.so").is_file())
        with patch("artifacts.time.time", return_value=failed.record["created"] + 8 * 86400):
            prune_snapshots(self.root)
        self.assertFalse(failed.root.exists())

    def test_packaged_install_copies_requested_plugin_without_loading_build_fixtures(self):
        plugin = self.root / "packaged library.so"
        plugin.write_text("packaged build")
        snapshot = self.snapshot(plugin=plugin, installation=True)
        self.assertEqual(digest(plugin), digest(snapshot.path("hyprspace.so")))
        with self.assertRaises(FileNotFoundError):
            snapshot.path("test-overview.so")

    def test_companions_and_providers_remain_in_the_run_generation(self):
        directory = self.root / "companions"
        (directory / "providers").mkdir(parents=True)
        paths = {name: directory / name for name in ("walker", "elephant", "providers/runner.so")}
        for name, path in paths.items():
            path.write_text("A:" + name)
        record = {"binaries": {name: digest(path) for name, path in paths.items()}}
        (directory / "compatibility.json").write_text(json.dumps(record))
        snapshot = self.snapshot(companions=directory)
        for name, path in paths.items():
            path.write_text("B:" + name)
            self.assertEqual(digest(snapshot.path("companions/" + name)), record["binaries"][name])
        with self.assertRaisesRegex(ValueError, "disagrees with compatibility record"):
            self.snapshot("mixed-companions", companions=directory)

    def test_changed_companion_manifest_cannot_mix_with_its_previous_binaries(self):
        directory = self.root / "companions"
        (directory / "providers").mkdir(parents=True)
        paths = {name: directory / name for name in ("walker", "elephant", "providers/runner.so")}
        for name, path in paths.items():
            path.write_text("A:" + name)
        record = {"binaries": {name: digest(path) for name, path in paths.items()}}
        manifest = directory / "compatibility.json"
        manifest.write_text(json.dumps(record))

        def changed(paths):
            manifest.write_text(json.dumps(record | {"generation": "B"}))
            return inventory(paths)

        with patch("artifacts.inventory", changed), self.assertRaisesRegex(ValueError, "inventory changed"):
            self.snapshot(companions=directory)

    def test_partial_fixture_reply_has_a_deadline(self):
        process = subprocess.Popen([sys.executable, "-c", "import os,time; os.write(1,b'partial'); time.sleep(10)"], stdout=subprocess.PIPE)
        try:
            with self.assertRaises(TimeoutError):
                reply(process, timeout=0.1)
        finally:
            process.kill()
            process.wait(timeout=3)
            process.stdout.close()

    def test_fixture_exit_is_distinct_from_timeout(self):
        process = subprocess.Popen([sys.executable, "-c", "pass"], stdout=subprocess.PIPE)
        try:
            with self.assertRaises(RuntimeError):
                reply(process)
        finally:
            process.wait(timeout=3)
            process.stdout.close()


if __name__ == "__main__":
    unittest.main()
