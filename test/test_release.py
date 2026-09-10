#!/usr/bin/env python3
"""Exercise release validation and archives in a disposable repository."""

import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch

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
                     "CHANGELOG.md", "LICENSE", "NOTICE.md", ".gitignore", "version.txt",
                     ".release-please-manifest.json", "release-please-config.json"]:
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(PROJECT / name, target)
        self.version, self.notes = RELEASE.check(self.root)
        self.tag = f"v{self.version}"
        (self.root / ".release-please-manifest.json").write_text(json.dumps({".": self.version}))
        (self.root / "CHANGELOG.md").write_text(f"# Changelog\n\n## {self.version} (2026-09-07)\n\n{self.notes}\n")
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

    def set_version(self, version):
        current = (self.root / "version.txt").read_text().strip()
        for name in ("hyprpm.toml", "src/Version.hpp", "version.txt",
                     ".release-please-manifest.json", "CHANGELOG.md"):
            path = self.root / name
            path.write_text(path.read_text().replace(current, version))

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

    def test_release_please_headings_and_subsections(self):
        for level in ("##", "###"):
            for title in (self.version, f"[{self.version}](https://example.com/compare)"):
                with self.subTest(level=level, title=title):
                    notes = "### Features\n\n- Add a feature.\n\n### Bug Fixes\n\n- Fix a bug."
                    (self.root / "CHANGELOG.md").write_text(
                        f"# Changelog\n\n{level} {title} (2026-09-07)\n\n{notes}\n\n"
                        "## 0.1.0\n\nEarlier notes.\n"
                    )
                    self.assertEqual(RELEASE.check(self.root, self.tag)[1], notes)

    def test_release_versions_stay_synchronized(self):
        for version in ("1.0.0", "1.0.1", "1.1.0", "2.0.0"):
            with self.subTest(version=version):
                self.set_version(version)
                self.assertEqual(RELEASE.check(self.root, f"v{version}")[0], version)

    def test_initial_manifest_allows_checks_but_not_tagging(self):
        config = json.loads((self.root / "release-please-config.json").read_text())
        initial = config["packages"]["."]["initial-version"]
        self.set_version(initial)
        (self.root / ".release-please-manifest.json").write_text('{".": "0.0.0"}')
        (self.root / "CHANGELOG.md").write_text("# Changelog\n\n## Unreleased\n\nFirst release.\n")
        self.assertEqual(RELEASE.check(self.root)[0], initial)
        with self.assertRaisesRegex(ValueError, "manifest"):
            RELEASE.check(self.root, f"v{initial}")

    def test_initial_manifest_rejects_later_versions(self):
        for version in ("1.0.1", "1.1.0", "2.0.0"):
            with self.subTest(version=version):
                self.set_version(version)
                (self.root / ".release-please-manifest.json").write_text('{".": "0.0.0"}')
                with self.assertRaisesRegex(ValueError, "manifest"):
                    RELEASE.check(self.root)

    def test_unreviewed_notes_and_duplicate_versions_are_rejected(self):
        path = self.root / "CHANGELOG.md"
        original = path.read_text()
        for extra in (f"## {self.version}\n\nDuplicate.", "## Unreleased\n\nPending."):
            path.write_text(original + "\n" + extra + "\n")
            with self.assertRaises(ValueError):
                RELEASE.check(self.root, self.tag)

    def test_manifest_and_version_file_mismatches(self):
        for name in ("version.txt", ".release-please-manifest.json"):
            path = self.root / name
            original = path.read_text()
            path.write_text(original.replace(self.version, "99.99.99"))
            with self.assertRaisesRegex(ValueError, "versions differ"):
                RELEASE.check(self.root)
            path.write_text(original)

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


DRAFT_SPEC = importlib.util.spec_from_file_location("release_draft", PROJECT / "scripts/release-draft.py")
DRAFT = importlib.util.module_from_spec(DRAFT_SPEC)
DRAFT_SPEC.loader.exec_module(DRAFT)


class DraftTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="hyprspace-draft-test-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        (self.root / "dist").mkdir()
        self.files = {"hyprspace-1.0.0.tar.gz": b"source archive", "SHA256SUMS": b"checksums"}
        for name, content in self.files.items():
            (self.root / "dist" / name).write_bytes(content)
        self.commit = "a" * 40
        self.release = {"id": 17, "draft": True, "assets": []}
        self.uploaded = []
        self.created = []
        self.remote_assets = {}
        self.fail_upload = False
        self.fail_package = False
        self.addCleanup(patch.stopall)
        patch.dict(os.environ, {"GH_REPO": "example/hyprspace"}).start()
        patch.object(DRAFT, "remote_commit", return_value=self.commit).start()
        patch.object(DRAFT, "command", side_effect=self.command).start()
        patch.object(DRAFT.subprocess, "run", side_effect=lambda *a, **k:
                     subprocess.CompletedProcess(a, 0, json.dumps(self.release).encode(), b"")).start()

    def command(self, *args):
        if args == ("git", "rev-parse", "HEAD"):
            return self.commit.encode()
        if args[:2] == ("bash", "scripts/dist.sh"):
            if self.fail_package:
                raise subprocess.CalledProcessError(1, args)
            return b""
        if args[0] == "python3":
            return b"Reviewed release notes"
        if args[:3] == ("gh", "release", "upload"):
            if self.fail_upload:
                raise subprocess.CalledProcessError(1, args)
            self.uploaded.append(Path(args[-1]).name)
            return b""
        if args[:3] == ("gh", "release", "create"):
            self.created.append(args)
            return b""
        if "Accept: application/octet-stream" in args:
            return self.remote_assets[int(args[-1].rsplit("/", 1)[1])]
        if args[:2] == ("gh", "api"):
            return json.dumps(self.release).encode()
        self.fail(f"unexpected command: {args}")

    def attach(self):
        DRAFT.attach("v1.0.0", self.commit, self.root)

    def test_empty_draft_gets_both_assets(self):
        self.attach()
        self.assertEqual(self.uploaded, list(self.files))

    def test_missing_draft_is_created_at_existing_tag(self):
        with patch.object(DRAFT.subprocess, "run", return_value=
                          subprocess.CompletedProcess([], 1, b"", b"gh: Not Found (HTTP 404)")):
            self.attach()
        self.assertEqual(len(self.created), 1)
        self.assertIn("--verify-tag", self.created[0])
        self.assertIn("--draft", self.created[0])
        self.assertIn(self.commit, self.created[0])
        self.assertEqual(self.uploaded, list(self.files))

    def test_api_failure_does_not_create_or_upload(self):
        with patch.object(DRAFT.subprocess, "run", return_value=
                          subprocess.CompletedProcess([], 1, b"", b"gh: Forbidden (HTTP 403)")):
            with self.assertRaisesRegex(ValueError, "Forbidden"):
                self.attach()
        self.assertEqual(self.created, [])
        self.assertEqual(self.uploaded, [])

    def test_incomplete_draft_only_uploads_missing_asset(self):
        self.release["assets"] = [{"id": 1, "name": "hyprspace-1.0.0.tar.gz"}]
        self.remote_assets[1] = self.files["hyprspace-1.0.0.tar.gz"]
        self.attach()
        self.assertEqual(self.uploaded, ["SHA256SUMS"])

    def test_completed_draft_retry_is_noop(self):
        for identifier, (name, content) in enumerate(self.files.items()):
            self.release["assets"].append({"id": identifier, "name": name})
            self.remote_assets[identifier] = content
        self.attach()
        self.assertEqual(self.uploaded, [])

    def test_conflicting_asset_is_not_replaced(self):
        self.release["assets"] = [{"id": 1, "name": "SHA256SUMS"}]
        self.remote_assets[1] = b"different checksums"
        with self.assertRaisesRegex(ValueError, "refusing replacement"):
            self.attach()
        self.assertEqual(self.uploaded, [])

    def test_published_release_is_not_modified(self):
        self.release["draft"] = False
        with self.assertRaisesRegex(ValueError, "published releases"):
            self.attach()
        self.assertEqual(self.uploaded, [])

    def test_failed_packaging_never_uploads(self):
        self.fail_package = True
        with self.assertRaises(subprocess.CalledProcessError):
            self.attach()
        self.assertEqual(self.uploaded, [])

    def test_failed_upload_can_be_retried(self):
        self.fail_upload = True
        with self.assertRaises(subprocess.CalledProcessError):
            self.attach()
        self.fail_upload = False
        self.attach()
        self.assertEqual(self.uploaded, list(self.files))

    def test_remote_tag_commit_mismatch_never_uploads(self):
        with patch.object(DRAFT, "remote_commit", return_value="b" * 40):
            with self.assertRaisesRegex(ValueError, "remote tag differs"):
                self.attach()
        self.assertEqual(self.uploaded, [])

    def test_tag_changed_during_packaging_never_uploads(self):
        with patch.object(DRAFT, "remote_commit", side_effect=[self.commit, "b" * 40]):
            with self.assertRaisesRegex(ValueError, "tag changed"):
                self.attach()
        self.assertEqual(self.uploaded, [])

    def test_artifact_job_requires_successful_exact_commit_checks(self):
        workflow = (PROJECT / ".github/workflows/release.yml").read_text()
        draft = workflow.split("  draft:\n", 1)[1]
        self.assertIn("needs: [resolve, checks]", draft)
        self.assertNotIn("always()", draft)
        self.assertNotIn("continue-on-error", workflow)
        self.assertIn("ref: ${{ needs.resolve.outputs.commit }}", draft)
        checks = workflow.split("  checks:\n", 1)[1].split("  draft:\n", 1)[0]
        self.assertIn("ref: ${{ needs.resolve.outputs.commit }}", checks)


if __name__ == "__main__":
    unittest.main()
