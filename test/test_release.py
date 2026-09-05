#!/usr/bin/env python3
"""Exercise release validation and archives in a disposable repository."""

import importlib.util
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location("release_check", PROJECT / "scripts/check-release.py")
RELEASE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RELEASE)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="hyprspace-release-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name in ["scripts/check-release.py", "scripts/dist.sh", "src/Version.hpp", "hyprpm.toml",
                     "CHANGELOG.md", "LICENSE", "NOTICE.md", ".gitignore"]:
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(PROJECT / name, target)
        self.version, self.notes = RELEASE.check(self.root)
        self.tag = f"v{self.version}"
        self.run_command("git", "init", "-q")
        self.run_command("git", "config", "core.hooksPath", "/dev/null")
        self.commit()
        self.run_command("git", "tag", self.tag)

    def run_command(self, *args, success=True):
        result = subprocess.run(args, cwd=self.root, text=True, capture_output=True, timeout=20)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def commit(self):
        self.run_command("git", "add", ".")
        self.run_command("git", "-c", "user.name=Release test", "-c", "user.email=test@localhost",
                         "-c", "commit.gpgsign=false", "commit", "-qm", "Release fixture")

    def test_metadata_and_notes(self):
        self.assertEqual(RELEASE.check(self.root, self.tag), (self.version, self.notes))
        result = self.run_command("python3", "scripts/check-release.py", "--tag", self.tag, "--notes")
        self.assertEqual(result.stdout.strip(), self.notes)

    def test_wrong_tag(self):
        for tag in ["v99.99.99", "1.0.0", "../../release", "v1.0.0; touch unexpected"]:
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                RELEASE.check(self.root, tag)

    def test_version_mismatch(self):
        path = self.root / "src/Version.hpp"
        path.write_text(path.read_text().replace(self.version, "99.99.99"))
        with self.assertRaisesRegex(ValueError, "versions differ"):
            RELEASE.check(self.root)

    def test_missing_notes(self):
        (self.root / "CHANGELOG.md").write_text(f"# Changelog\n\n## {self.version}\n\n")
        with self.assertRaisesRegex(ValueError, "needs release notes"):
            RELEASE.check(self.root)

    def test_dirty_checkout(self):
        (self.root / "private-note.txt").write_text("Uncommitted content")
        result = self.run_command("bash", "scripts/dist.sh", self.tag, success=False)
        self.assertIn("Commit the release files", result.stderr)
        self.assertFalse((self.root / "dist").exists())

    def test_different_commit(self):
        (self.root / "later.txt").write_text("Later change")
        self.commit()
        result = self.run_command("bash", "scripts/dist.sh", self.tag, success=False)
        self.assertIn("Check out the version tag", result.stderr)

    def test_missing_tag(self):
        self.run_command("git", "tag", "-d", self.tag)
        self.run_command("bash", "scripts/dist.sh", self.tag, success=False)
        self.assertFalse((self.root / "dist").exists())

    def test_archive_is_reproducible_and_excludes_local_builds(self):
        (self.root / "build").mkdir()
        (self.root / "build/hyprspace.so").write_text("Local binary")
        self.run_command("bash", "scripts/dist.sh", self.tag)
        archive = self.root / f"dist/hyprspace-{self.version}.tar.gz"
        first = archive.read_bytes()
        with tarfile.open(archive) as source:
            names = source.getnames()
        prefix = f"hyprspace-{self.version}/"
        self.assertIn(prefix + "LICENSE", names)
        self.assertIn(prefix + "NOTICE.md", names)
        self.assertIn(prefix + "hyprpm.toml", names)
        self.assertTrue(all(name == prefix.rstrip("/") or name.startswith(prefix) for name in names))
        self.assertFalse(any(name.endswith(".so") or "/.git/" in name for name in names))
        self.run_command("bash", "scripts/dist.sh", self.tag)
        self.assertEqual(first, archive.read_bytes())
        checksum = subprocess.run(["sha256sum", "--check", "SHA256SUMS"], cwd=self.root / "dist",
                                  capture_output=True, text=True, timeout=5)
        self.assertEqual(checksum.returncode, 0, checksum.stdout + checksum.stderr)


if __name__ == "__main__":
    unittest.main()
