#!/usr/bin/env python3
"""Check version metadata and extract the matching changelog entry."""

import argparse
import json
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
    if (root / "version.txt").read_text().strip() != version:
        raise ValueError("version.txt and hyprpm.toml versions differ")
    released = json.loads((root / ".release-please-manifest.json").read_text())
    initial = json.loads((root / "release-please-config.json").read_text())["packages"]["."]["initial-version"]
    bootstrap = released == {".": "0.0.0"} and version == initial and tag is None
    if not bootstrap and released != {".": version}:
        raise ValueError("release manifest and plugin versions differ")
    if tag is not None and tag != f"v{version}":
        raise ValueError(f"expected tag v{version}, got {tag!r}")
    changelog = (root / "CHANGELOG.md").read_text()
    # Release Please uses H2 for feature releases and H3 for patch releases,
    # with either a plain version or a comparison link, followed by a date.
    heading = re.compile(
        r"^#{2,3} (?:\[v?(\d+\.\d+\.\d+)\]\([^\n)]+\)|v?(\d+\.\d+\.\d+)|Unreleased)"
        r"(?: \(\d{4}-\d{2}-\d{2}\))?\s*$", re.M
    )
    entries = {}
    matches = list(heading.finditer(changelog))
    for index, match in enumerate(matches):
        key = match[1] or match[2] or "Unreleased"
        if key in entries:
            raise ValueError(f"duplicate changelog entry: {key}")
        end = matches[index + 1].start() if index + 1 < len(matches) else len(changelog)
        entries[key] = changelog[match.end():end].strip()
    key = "Unreleased" if bootstrap else version
    if not entries.get(key):
        raise ValueError(f"CHANGELOG.md needs release notes for {key}")
    if not bootstrap and entries.get("Unreleased"):
        raise ValueError("move the Unreleased notes into the release entry before releasing")
    return version, entries[key]


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
