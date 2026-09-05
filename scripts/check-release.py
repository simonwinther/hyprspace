#!/usr/bin/env python3
"""Check version metadata and extract the matching changelog entry."""

import argparse
from pathlib import Path
import re
import sys
import tomllib


def check(root, tag=None):
    manifest = tomllib.loads((root / "hyprpm.toml").read_text())
    version = manifest["hyprspace"]["version"]
    if not re.fullmatch(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)", version):
        raise ValueError("hyprpm.toml must contain a stable X.Y.Z version")
    source = (root / "src/Version.hpp").read_text()
    match = re.search(r'\bVERSION\[\]\s*=\s*"([^"]+)";', source)
    if not match or match[1] != version:
        raise ValueError("src/Version.hpp and hyprpm.toml versions differ")
    if tag is not None and tag != f"v{version}":
        raise ValueError(f"expected tag v{version}, got {tag!r}")
    changelog = (root / "CHANGELOG.md").read_text()
    entry = re.search(rf"^## {re.escape(version)}\n(.*?)(?=^## |\Z)", changelog, re.M | re.S)
    if not entry or not entry[1].strip():
        raise ValueError(f"CHANGELOG.md needs release notes under '## {version}'")
    return version, entry[1].strip()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag")
    parser.add_argument("--notes", action="store_true")
    args = parser.parse_args()
    try:
        version, notes = check(Path(__file__).resolve().parent.parent, args.tag)
    except (OSError, KeyError, ValueError) as error:
        sys.exit(f"release check: {error}")
    print(notes if args.notes else f"Release metadata OK: v{version}")
