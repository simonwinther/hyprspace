#!/usr/bin/env python3
"""Build the locked Nix package and verify its files and compositor agreement."""

import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
NIX = ['nix', '--extra-experimental-features', 'nix-command flakes']


def run(*args, **kwargs):
    return subprocess.check_output(args, text=True, **kwargs).strip()


def evaluate(expression):
    return run(*NIX, 'eval', '--impure', '--raw', '--expr', expression)


def main():
    os.chdir(ROOT)
    plugin = Path(run(*NIX, 'build', '--no-write-lock-file', '--no-link', '--print-out-paths', '.#hyprspace'))
    prefix = f'let f = builtins.getFlake {json.dumps(str(ROOT))}; p = f.packages.x86_64-linux.hyprspace; in '
    compositor = Path(evaluate(prefix + 'p.compositor.outPath'))
    assert evaluate(prefix + 'p.stdenv.cc.outPath') == evaluate(prefix + 'p.compositor.stdenv.cc.outPath')
    assert evaluate(prefix + 'f.packages.x86_64-linux.default.outPath') == str(plugin)
    library = plugin / 'lib/libhyprspace.so'
    assert library.is_file()
    helper = plugin / 'lib/hyprspace-launch'
    interpreter = helper.read_text().splitlines()[0].removeprefix('#!')
    assert interpreter.startswith('/nix/store/') and Path(interpreter).is_file()
    assert (plugin / 'bin/hyprspace-launch').resolve() == helper
    for name in ('uwsm-app', 'uwsm', 'app2unit'):
        assert (plugin / 'lib/launch-bin' / name).resolve() == helper
    assert (plugin / 'share/hyprspace/bindings.conf').read_bytes() == (ROOT / 'contrib/bindings.conf').read_bytes()
    helper_env = os.environ.copy()
    for key in ("XDG_RUNTIME_DIR", "HYPRLAND_INSTANCE_SIGNATURE"):
        helper_env.pop(key, None)
    assert run(str(helper), '--', interpreter, '-c', 'print("helper works")', env=helper_env) == 'helper works'
    metadata = run('readelf', '--string-dump=.hyprspace.abi', str(library))
    match = re.search(r'([0-9a-f]{40})_aq_([\d.]+)_hu_([\d.]+)_hg_([\d.]+)_hc_([\d.]+)_hlg_([\d.]+)', metadata)
    assert match, metadata
    abi = match[1] + ''.join(f'_{label}_{value.rsplit(".", 1)[0]}' for label, value in zip(('aq', 'hu', 'hg', 'hc', 'hlg'), match.groups()[1:]))
    # Hyprland initializes its runtime path even for --version-json.
    with tempfile.TemporaryDirectory(prefix='hyprspace-nix-check-') as runtime:
        version_env = helper_env | {'XDG_RUNTIME_DIR': runtime}
        installed = json.loads(run(str(compositor / 'bin/Hyprland'), '--version-json', env=version_env))
    assert abi == installed['abiHash'], (abi, installed)
    # Without this function entry the upstream trampoline copies a relative
    # call without relocating it, crashing on the first pointer operation.
    symbol = '_ZN13CInputManager22getMouseCoordsInternalEv'
    assembly = run('objdump', '-d', f'--disassemble={symbol}',
                   str(compositor / 'bin/.Hyprland-wrapped'))
    assert re.search(rf'<{symbol}>:\n\s*[0-9a-f]+:\s+f3 0f 1e fa\s+endbr64', assembly), assembly
    unsupported = f'((import {ROOT / "flake.nix"}).outputs {{ self = {{}}; hyprland = {{ rev = "unsupported"; }}; }}).packages.x86_64-linux.hyprspace.drvPath'
    rejected = subprocess.run([*NIX, 'eval', '--impure', '--raw', '--expr', unsupported], text=True, capture_output=True)
    assert rejected.returncode != 0 and 'supports only Hyprland 0.56.2' in rejected.stderr, rejected
    print(f'Nix package, helpers, compiler and ABI verified: {plugin}')
    print(f'Compositor: {compositor}')


if __name__ == '__main__':
    main()
