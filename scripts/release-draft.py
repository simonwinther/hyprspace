#!/usr/bin/env python3
"""Attach verified source assets to a draft, resuming incomplete uploads safely."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def command(*args):
    return subprocess.check_output(args)


def remote_commit(repo, tag):
    ref = json.loads(command('gh', 'api', f'repos/{repo}/git/ref/tags/{tag}'))['object']
    while ref['type'] == 'tag':
        ref = json.loads(command('gh', 'api', f"repos/{repo}/git/tags/{ref['sha']}"))['object']
    if ref['type'] != 'commit':
        raise ValueError('release tag must point to a commit')
    return ref['sha']


def attach(tag, commit, root):
    if not re.fullmatch(r'v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)', tag):
        raise ValueError('expected a stable vX.Y.Z tag')
    if not re.fullmatch(r'[0-9a-f]{40}', commit):
        raise ValueError('expected the full checked commit')
    repo = os.environ['GH_REPO']
    if command('git', 'rev-parse', 'HEAD').decode().strip() != commit:
        raise ValueError('checkout differs from the checked commit')
    if remote_commit(repo, tag) != commit:
        raise ValueError('remote tag differs from the checked commit')
    # dist.sh independently verifies the clean checkout and local tag.
    command('bash', 'scripts/dist.sh', tag)
    notes = command('python3', 'scripts/check-release.py', '--tag', tag, '--notes')
    result = subprocess.run(
        ['gh', 'api', f'repos/{repo}/releases/tags/{tag}'], capture_output=True
    )
    if result.returncode:
        if b'HTTP 404' not in result.stderr:
            raise ValueError(result.stderr.decode())
        notes_file = root / 'dist/release-notes.md'
        notes_file.write_bytes(notes)
        command('gh', 'release', 'create', tag, '--verify-tag', '--draft',
                '--target', commit, '--title', f'hyprspace {tag}',
                '--notes-file', str(notes_file))
        release = json.loads(command('gh', 'api', f'repos/{repo}/releases/tags/{tag}'))
    else:
        release = json.loads(result.stdout)
    if not release['draft']:
        raise ValueError('published releases and their assets cannot be replaced')
    assets = {asset['name']: asset for asset in release['assets']}
    pending = []
    for name in (f'hyprspace-{tag[1:]}.tar.gz', 'SHA256SUMS'):
        path = root / 'dist' / name
        if name in assets:
            content = command('gh', 'api', '-H', 'Accept: application/octet-stream',
                              f"repos/{repo}/releases/assets/{assets[name]['id']}")
            if hashlib.sha256(content).digest() != hashlib.sha256(path.read_bytes()).digest():
                raise ValueError(f'existing draft asset differs: {name}; refusing replacement')
        else:
            pending.append(path)
    if remote_commit(repo, tag) != commit:
        raise ValueError('remote tag changed during packaging')
    # Recheck publication before writing; upload never uses --clobber.
    current = json.loads(command('gh', 'api', f"repos/{repo}/releases/{release['id']}"))
    if not current['draft']:
        raise ValueError('release was published during packaging')
    for path in pending:
        command('gh', 'release', 'upload', tag, str(path))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--commit', required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    os.chdir(root)
    try:
        attach(args.tag, args.commit, root)
    except (ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'draft release: {error}\n')
