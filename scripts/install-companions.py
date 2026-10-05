#!/usr/bin/env python3
"""Stage verified companions and reversibly activate personal launcher overrides."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
SESSION_ENV = ("WAYLAND_DISPLAY", "DISPLAY", "XDG_RUNTIME_DIR", "HYPRLAND_INSTANCE_SIGNATURE",
               "DBUS_SESSION_BUS_ADDRESS", "GSK_RENDERER", "XDG_CURRENT_DESKTOP",
               "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def atomic_write(path, body, mode=0o600):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as output:
        temporary = Path(output.name)
        output.write(body)
    try:
        temporary.chmod(mode)
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def write_json(path, value):
    atomic_write(path, (json.dumps(value, indent=2) + "\n").encode())


def verify(source, provider_dir):
    record = json.loads((source / "compatibility.json").read_text())
    versions = json.loads((ROOT / "companion/versions.json").read_text())
    for name, expected in versions.items():
        if any(record[name].get(key) != value for key, value in expected.items()):
            raise ValueError(f"{name}: companion source metadata does not match the pinned build")
        patch = ROOT / f'companion/{name}-{expected["version"]}.patch'
        if record[name].get("patch_sha256") != digest(patch):
            raise ValueError(f"{name}: companion patch differs from this checkout")
    files = record.get("binaries", {})
    if not {"walker", "elephant"}.issubset(files):
        raise ValueError("compatibility record must include both executables")
    for name, expected in files.items():
        relative = Path(name)
        if relative.is_absolute() or ".." in relative.parts or not (source / name).is_file() or digest(source / name) != expected:
            raise ValueError(f"{name}: companion binary checksum mismatch")
    missing = {f"providers/{path.name}" for path in provider_dir.glob("*.so")} - files.keys()
    if missing:
        raise ValueError("rebuild all installed providers with the companion daemon: " + ", ".join(sorted(missing)))
    return record


def stage(source, helper, root, provider_dir):
    source, helper, root = source.resolve(), helper.resolve(), root.resolve()
    record = verify(source, provider_dir)
    helper_hash = digest(helper)
    identity = hashlib.sha256((json.dumps(record, sort_keys=True) + helper_hash).encode()).hexdigest()[:16]
    release = root / "releases" / identity
    if release.exists():
        saved = json.loads((release / "stage.json").read_text())
        if saved["compatibility"] != record or saved["helper_sha256"] != helper_hash:
            raise ValueError("existing staged release does not match the build")
        verify(release / "bin", provider_dir)
        if digest(release / "launch-assets/hyprspace-launch") != helper_hash:
            raise ValueError("staged launch helper changed")
        return release
    release.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".stage-", dir=release.parent) as temporary:
        prepared = Path(temporary) / "release"
        for name in record["binaries"]:
            target = prepared / "bin" / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source / name, target)
        write_json(prepared / "bin/compatibility.json", record)
        assets = prepared / "launch-assets"
        (assets / "launch-bin").mkdir(parents=True)
        shutil.copy2(helper, assets / "hyprspace-launch")
        (assets / "hyprspace-launch").chmod(0o755)
        for name in ("uwsm-app", "uwsm", "app2unit"):
            (assets / "launch-bin" / name).symlink_to("../hyprspace-launch")
        verify(prepared / "bin", provider_dir)
        if digest(assets / "hyprspace-launch") != helper_hash:
            raise ValueError("launch helper changed while staging")
        write_json(prepared / "stage.json", {"compatibility": record, "helper_sha256": helper_hash})
        prepared.rename(release)
    return release


def wrapper(release, executable):
    prefix = str(release / "bin") + ":" + str(release / "launch-assets")
    return (
        "#!/bin/sh\n"
        f"export PATH={shlex.quote(prefix)}:\"$PATH\"\n"
        f"export ELEPHANT_PROVIDER_DIR={shlex.quote(str(release / 'bin/providers'))}\n"
        f"exec {shlex.quote(str(executable))} \"$@\"\n"
    ).encode()


def service_start(unit):
    section, commands, pending = "", [], ""
    for raw in unit.splitlines():
        line = pending + raw.strip()
        if line.endswith("\\"):
            pending = line[:-1] + " "
            continue
        pending = ""
        if line.startswith("["):
            section = line
        elif section == "[Service]" and line.startswith("ExecStart="):
            value = line.split("=", 1)[1].strip()
            commands = [*commands, value] if value else []
    if len(commands) != 1:
        raise ValueError("Elephant requires exactly one ExecStart command")
    match = re.fullmatch(r'(-?)("[^"\n]+"|\'[^\'\n]+\'|[^\s]+)(.*)', commands[0])
    if not match or Path(shlex.split(match[2])[0]).name != "elephant":
        raise ValueError("Elephant ExecStart must directly execute elephant; preserve custom wrappers manually")
    return match[1], match[3]


def service_override(executable, unit):
    prefix, tail = service_start(unit)
    path = str(executable).replace("\\", "\\\\").replace('"', '\\"').replace("%", "%%")
    return f'[Service]\nExecStart=\nExecStart={prefix}"{path}"{tail}\n'.encode()


def snapshot(path, backup):
    if path.is_symlink():
        return {"kind": "symlink", "target": os.readlink(path)}
    if not path.exists():
        return {"kind": "absent"}
    if not path.is_file():
        raise ValueError(f"{path}: expected a file or symlink")
    shutil.copy2(path, backup)
    return {"kind": "file", "backup": str(backup), "mode": path.stat().st_mode & 0o777}


def walker_services():
    result = []
    for process in Path("/proc").iterdir():
        if not process.name.isdigit():
            continue
        try:
            if process.stat().st_uid != os.getuid():
                continue
            argv = [part.decode() for part in (process / "cmdline").read_bytes().split(b"\0") if part]
            if not argv or Path(argv[0]).name != "walker" or "--gapplication-service" not in argv:
                continue
            environ = dict(part.split(b"=", 1) for part in (process / "environ").read_bytes().split(b"\0") if b"=" in part)
            result.append({"pid": int(process.name), "argv": argv,
                           "env": {key: environ[key.encode()].decode() for key in SESSION_ENV if key.encode() in environ}})
        except (OSError, UnicodeError):
            continue
    return result


def stop_walkers(services):
    for saved in services:
        # Recheck the exact process before signalling; a reused PID is unrelated.
        if not any(item["pid"] == saved["pid"] and item["argv"] == saved["argv"] for item in walker_services()):
            continue
        os.kill(saved["pid"], signal.SIGTERM)
    deadline = time.monotonic() + 5
    while any(item["pid"] in {saved["pid"] for saved in services} for item in walker_services()):
        if time.monotonic() >= deadline:
            raise TimeoutError("Walker service did not stop; no unrelated processes were signalled")
        time.sleep(0.05)


def start_walkers(services, executable, log):
    for saved in services:
        if executable is None and any(item["argv"] == saved["argv"] for item in walker_services()):
            continue
        env = os.environ.copy()
        env.update(saved["env"])
        with log.open("ab") as output:
            subprocess.Popen([str(executable) if executable else saved["argv"][0], *saved["argv"][1:]],
                             env=env, stdin=subprocess.DEVNULL, stdout=output, stderr=output, start_new_session=True)


def activate(release, root, bin_dir, config_dir):
    active = root / "active.json"
    if active.exists():
        saved = json.loads(active.read_text())
        if saved["release"] == str(release):
            return Path(saved["manifest"])
        raise ValueError("another companion release is active; rollback it before activating this release")
    unit = subprocess.check_output(["systemctl", "--user", "cat", "elephant.service"], text=True)
    dropin = config_dir / "systemd/user/elephant.service.d/zz-hyprspace-companions.conf"
    elephant = bin_dir / "elephant"
    plans = [(bin_dir / name, wrapper(release, release / "bin" / name), 0o755) for name in ("walker", "elephant")]
    plans.append((dropin, service_override(elephant, unit), 0o644))
    transaction = root / "rollbacks" / f"{release.name}-{time.time_ns()}"
    transaction.mkdir(parents=True, mode=0o700)
    launcher = bin_dir / "omarchy-launch-walker"
    original = launcher if launcher.exists() else Path.home() / ".local/share/omarchy/bin/omarchy-launch-walker"
    if original.is_file():
        delegate = transaction / "original-omarchy-launch-walker"
        shutil.copy2(original, delegate)
        delegate.chmod(0o755)
        plans.append((launcher, wrapper(release, delegate), 0o755))
    services = walker_services()
    if len(services) > 1:
        raise ValueError("multiple Walker services are running; resolve the duplicate before activation")
    running = subprocess.run(["systemctl", "--user", "is-active", "--quiet", "elephant.service"]).returncode == 0
    manifest = transaction / "manifest.json"
    state = {"release": str(release), "root": str(root), "walker_services": services, "elephant_active": running,
             "files": [], "installed": [], "status": "prepared"}
    for index, (path, body, mode) in enumerate(plans):
        state["files"].append({"path": str(path), "original": snapshot(path, transaction / f"backup-{index}"),
                               "installed_sha256": hashlib.sha256(body).hexdigest(), "mode": mode})
    write_json(manifest, state)
    try:
        for entry, (path, body, mode) in zip(state["files"], plans):
            atomic_write(path, body, mode)
            state["installed"].append(entry["path"])
            write_json(manifest, state)
        stop_walkers(services)
        subprocess.run(["systemctl", "--user", "daemon-reload"], check=True)
        if running:
            subprocess.run(["systemctl", "--user", "restart", "elephant.service"], check=True)
        start_walkers(services, bin_dir / "walker", transaction / "walker.log")
        state["status"] = "active"
        write_json(manifest, state)
        write_json(active, {"release": str(release), "manifest": str(manifest)})
    except Exception:
        rollback(manifest, check_changes=False)
        raise
    return manifest


def rollback(manifest, check_changes=True):
    state = json.loads(manifest.read_text())
    if state["status"] == "rolled-back":
        return
    entries = [entry for entry in state["files"] if entry["path"] in state["installed"]]
    if check_changes:
        for entry in entries:
            path = Path(entry["path"])
            if path.is_symlink() or not path.is_file() or digest(path) != entry["installed_sha256"]:
                raise ValueError(f"{path}: changed after installation; restore its installed version before rollback")
    stop_walkers([item for item in walker_services() if Path(item["argv"][0]).resolve() == Path(state["release"]) / "bin/walker"])
    for entry in reversed(entries):
        path, original = Path(entry["path"]), entry["original"]
        if original["kind"] == "file":
            atomic_write(path, Path(original["backup"]).read_bytes(), original["mode"])
        else:
            path.unlink(missing_ok=True)
            if original["kind"] == "symlink":
                path.symlink_to(original["target"])
    subprocess.run(["systemctl", "--user", "daemon-reload"], check=True)
    if state["elephant_active"]:
        subprocess.run(["systemctl", "--user", "restart", "elephant.service"], check=True)
    start_walkers(state["walker_services"], None, manifest.parent / "walker-rollback.log")
    state["status"] = "rolled-back"
    write_json(manifest, state)
    active = Path(state["root"]) / "active.json"
    if active.exists() and json.loads(active.read_text())["manifest"] == str(manifest):
        active.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "build/companions/bin")
    parser.add_argument("--helper", type=Path, default=ROOT / "build/hyprspace-launch")
    parser.add_argument("--root", type=Path, default=Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")) / "hyprspace/companions")
    parser.add_argument("--bin-dir", type=Path, default=Path.home() / ".local/bin")
    parser.add_argument("--config-dir", type=Path, default=Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")))
    parser.add_argument("--installed-provider-dir", type=Path, default=Path("/usr/lib/elephant"))
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--activate", action="store_true", help="write personal overrides and restart the existing launcher services")
    action.add_argument("--rollback", type=Path, help="restore the exact files and service commands saved by this manifest")
    args = parser.parse_args()
    if args.rollback:
        rollback(args.rollback.resolve())
        print("Companion overrides rolled back.")
        return
    release = stage(args.source, args.helper, args.root, args.installed_provider_dir)
    print(f"Verified companions staged: {release}")
    if args.activate:
        manifest = activate(release, args.root.resolve(), args.bin_dir.absolute(), args.config_dir.absolute())
        print(f"Rollback manifest: {manifest}")
    else:
        print("Launcher files and services are unchanged. Use --activate after companion integration passes.")


if __name__ == "__main__":
    main()
