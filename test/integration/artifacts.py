"""Publish a checked build inventory, then copy it once before starting a run.

Copies are separate inodes, never hardlinks. A concurrent build can make snapshot
creation fail, but cannot change a running snapshot or silently supply a missing
file. Fixture markers bind C++ plugins to the exact main library used at build.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import uuid


BINARIES = ("hyprspace.so", "test-overview.so", "test-dispatchers.so", "test-headless",
            "test-pointer", "test-ime", "test-activation", "test-lock", "hyprspace-launch")
FIXTURES = ("test-overview.so", "test-dispatchers.so")
MARKER = b"hyprspace-test-plugin-sha256:"


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def inventory(paths):
    return {name: {"source": str(path.resolve()), "sha256": digest(path)}
            for name, path in sorted(paths.items())}


def verify_fixtures(paths):
    expected = MARKER + digest(paths["hyprspace.so"]).encode() + b"\0"
    for name in FIXTURES:
        if name in paths and expected not in paths[name].read_bytes():
            raise ValueError(f"{name} was built against a different plugin; rebuild integration-fixtures")


def publish(repo, build, library_path=None):
    paths = {name: build / name for name in BINARIES}
    paths.update({name: repo / "test/integration" / name for name in ("client.py", "layer.py")})
    paths["bindings.conf"] = repo / "contrib/bindings.conf"
    paths["build-config"] = build / ".build-config"
    paths["companion-versions.json"] = repo / "companion/versions.json"
    for name, spec in json.loads(paths["companion-versions.json"].read_text()).items():
        patch = f'{name}-{spec["version"]}.patch'
        paths[patch] = repo / "companion" / patch
    # The private wlroots host can use development libraries inside build/.
    # Copy those dependencies too; the host searches $ORIGIN/lib.
    env = os.environ.copy()
    if library_path:
        env["LD_LIBRARY_PATH"] = str(library_path)
    libraries = subprocess.check_output(["ldd", str(build / "test-headless")], env=env, text=True, timeout=10)
    if "not found" in libraries:
        raise ValueError(f"Unresolved display-host dependencies:\n{libraries}")
    for line in libraries.splitlines():
        words = line.split()
        if len(words) >= 3 and words[1] == "=>" and words[2].startswith("/"):
            path = Path(words[2])
            if not path.resolve().is_relative_to("/usr"):
                paths["lib/" + words[0]] = path
    verify_fixtures(paths)
    files = inventory(paths)
    verify_fixtures(paths)
    if files != inventory(paths):
        raise ValueError("Build changed while publishing the integration inventory")
    record = {"schema": 1, "build": uuid.uuid4().hex, "files": files,
              "revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip(),
              "dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=repo, text=True))}
    destination = build / "integration.json"
    with tempfile.NamedTemporaryFile(mode="w", dir=build, delete=False) as output:
        temporary = Path(output.name)
        try:
            json.dump(record, output, indent=2)
            output.close()
            temporary.replace(destination)
        finally:
            temporary.unlink(missing_ok=True)


class Snapshot:
    def __init__(self, root, record):
        self.root = root
        self.record = record

    @classmethod
    def create(cls, root, manifest, plugin=None, installation=False, companions=None):
        record = json.loads(manifest.read_text())
        if record["schema"] != 1:
            raise ValueError("Unsupported integration inventory")
        files = record["files"].copy()
        missing = set(BINARIES) - files.keys()
        if missing:
            raise ValueError(f"Incomplete integration inventory: {sorted(missing)}")
        if plugin:
            files["hyprspace.so"] = inventory({"hyprspace.so": plugin})["hyprspace.so"]
            if installation:
                for name in FIXTURES:
                    files.pop(name)
        if companions:
            compatibility_bytes = (companions / "compatibility.json").read_bytes()
            compatibility = json.loads(compatibility_bytes)
            if not {"walker", "elephant", "providers/runner.so"} <= compatibility["binaries"].keys():
                raise ValueError("Incomplete companion inventory")
            extra = inventory({"companions/" + name: companions / name
                               for name in (*compatibility["binaries"], "compatibility.json")})
            if extra["companions/compatibility.json"]["sha256"] != hashlib.sha256(compatibility_bytes).hexdigest():
                raise ValueError("Companion inventory changed during snapshot preparation")
            for name, expected in compatibility["binaries"].items():
                if extra["companions/" + name]["sha256"] != expected:
                    raise ValueError(f"Companion binary disagrees with compatibility record: {name}")
            files.update(extra)
        destination = root / "artifacts"
        destination.mkdir()
        try:
            for name, spec in files.items():
                target = destination / name
                if not target.resolve().is_relative_to(destination.resolve()):
                    raise ValueError("Invalid artifact path")
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(spec["source"], target)
                if digest(target) != spec["sha256"]:
                    raise ValueError(f"Build changed or is incomplete: {name}; rebuild integration-fixtures")
                target.chmod(0o555)
            verify_fixtures({name: destination / name for name in files})
            (destination / "launch-bin").mkdir()
            for name in ("uwsm", "uwsm-app", "app2unit"):
                (destination / "launch-bin" / name).symlink_to("../hyprspace-launch")
            record = record | {"generation": uuid.uuid4().hex, "files": files,
                               "snapshot": str(destination), "created": time.time()}
            (root / "generation.json").write_text(json.dumps(record, indent=2))
            return cls(destination, record)
        except BaseException:
            shutil.rmtree(destination)
            raise

    @classmethod
    def attach(cls, root):
        record = json.loads((root / "generation.json").read_text())
        snapshot = cls(Path(record["snapshot"]), record)
        for name, spec in record["files"].items():
            if digest(snapshot.path(name)) != spec["sha256"]:
                raise ValueError(f"Snapshot changed: {name}")
        return snapshot

    def path(self, name):
        if name not in self.record["files"] or not (self.root / name).is_file():
            raise FileNotFoundError(f"Required snapshot artifact is missing: {name}")
        return self.root / name

    def finish(self, failed):
        if failed:
            # Failure binaries accompany logs for seven days. A later runner
            # prunes only completed, expired snapshots, never an active run.
            (self.root / "finished").write_text(str(time.time()))
        else:
            shutil.rmtree(self.root)


def prune_snapshots(parent):
    for marker in parent.glob("hs-i.*/artifacts/finished"):
        if marker.stat().st_uid == os.getuid() and time.time() - marker.stat().st_mtime > 7 * 86400:
            shutil.rmtree(marker.parent)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--library-path", type=Path)
    args = parser.parse_args()
    publish(Path(__file__).resolve().parents[2], args.build.resolve(), args.library_path)
