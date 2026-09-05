#!/usr/bin/env python3
"""Build pinned Walker/Elephant companions into an isolated directory."""

import argparse
import hashlib
import json
import re
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def run(*args, cwd=None):
    subprocess.run(args, cwd=cwd, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/companions")
    parser.add_argument("--go", default="go")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument(
        "--provider",
        action="append",
        default=[],
        help="also build this provider from the pinned daemon source",
    )
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    versions = json.loads((ROOT / "companion/versions.json").read_text())
    for name, spec in versions.items():
        archive = output / f'{name}-{spec["version"]}.tar.gz'
        if not archive.exists():
            with urllib.request.urlopen(spec["archive"], timeout=60) as response:
                archive.write_bytes(response.read())
        if hashlib.sha256(archive.read_bytes()).hexdigest() != spec["sha256"]:
            raise SystemExit(f"{archive}: checksum mismatch; remove it before retrying")
        patch = ROOT / f'companion/{name}-{spec["version"]}.patch'
        spec["patch_sha256"] = hashlib.sha256(patch.read_bytes()).hexdigest()
        source = output / f"{name}-src"
        stamp = source / ".hyprspace-companion"
        expected = json.dumps(spec, sort_keys=True)
        if source.exists() and (not stamp.exists() or stamp.read_text() != expected):
            raise SystemExit(
                f"{source}: existing source differs; select a fresh --output directory"
            )
        if not source.exists():
            unpack = output / f"{name}-unpack"
            unpack.mkdir(exist_ok=True)
            with tarfile.open(archive) as tar:
                tar.extractall(unpack, filter="data")
            folders = list(unpack.iterdir())
            if len(folders) != 1 or not folders[0].is_dir():
                raise SystemExit(f"{archive}: unexpected archive structure")
            folders[0].rename(source)
            unpack.rmdir()
            with patch.open("rb") as body:
                subprocess.run(
                    ["patch", "--batch", "--fuzz=0", "-p1"],
                    stdin=body,
                    cwd=source,
                    check=True,
                )
            stamp.write_text(expected)
    if args.prepare_only:
        return
    binary = output / "bin"
    providers = binary / "providers"
    providers.mkdir(parents=True, exist_ok=True)
    run(
        "cargo",
        "test",
        "--locked",
        "-j",
        str(args.jobs),
        "launch_context",
        cwd=output / "walker-src",
    )
    run(
        "cargo",
        "build",
        "--release",
        "--locked",
        "-j",
        str(args.jobs),
        cwd=output / "walker-src",
    )
    shutil.copy2(output / "walker-src/target/release/walker", binary / "walker.new")
    (binary / "walker.new").replace(binary / "walker")
    run(
        args.go,
        "test",
        "./pkg/common",
        "./internal/comm/handlers",
        cwd=output / "elephant-src",
    )
    run(
        args.go,
        "build",
        "-o",
        str(binary / "elephant"),
        "./cmd/elephant",
        cwd=output / "elephant-src",
    )
    # Go plugins must share the daemon's sources, compiler and dependency graph.
    for provider in sorted(
        set(
            [
                "desktopapplications",
                "websearch",
                "runner",
                *args.provider,
                *(path.stem for path in providers.glob("*.so")),
            ]
        )
    ):
        if (
            not re.fullmatch(r"[a-z0-9_-]+", provider)
            or not (output / "elephant-src/internal/providers" / provider).is_dir()
        ):
            raise SystemExit(f"unknown provider: {provider}")
        run(
            args.go,
            "build",
            "-buildmode=plugin",
            "-o",
            str(providers / f"{provider}.so"),
            f"./internal/providers/{provider}",
            cwd=output / "elephant-src",
        )
    record = dict(versions)
    record["toolchains"] = {
        name: subprocess.check_output(command, text=True).strip()
        for name, command in {
            "go": [args.go, "version"],
            "rust": ["rustc", "--version"],
            "cargo": ["cargo", "--version"],
        }.items()
    }
    record["binaries"] = {
        str(path.relative_to(binary)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in [
            binary / "walker",
            binary / "elephant",
            *sorted(providers.glob("*.so")),
        ]
    }
    (binary / "compatibility.json").write_text(json.dumps(record, indent=2) + "\n")
    print(f"Companions built in {binary}; no installed application was changed.")


if __name__ == "__main__":
    main()
