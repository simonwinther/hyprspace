#!/usr/bin/env bash
# Run a producer with a temporary output path as its final argument. Publish
# only complete files; an existing inode may still be mapped by Hyprland.
set -euo pipefail

destination=${1:?expected an output path}
shift
[[ $# -gt 0 ]] || { echo "error: expected an output command" >&2; exit 1; }

output_dir=$(dirname -- "$destination")
mkdir -p -- "$output_dir"
output_tmp=$(mktemp -d -- "$output_dir/.hyprspace-output.XXXXXX")
trap 'rm -f -- "$output_tmp/output"; rmdir -- "$output_tmp"' EXIT
trap 'exit 130' INT
trap 'exit 143' HUP TERM

"$@" "$output_tmp/output"
[[ -f "$output_tmp/output" ]] || { echo "error: command did not produce $destination" >&2; exit 1; }
mv -fT -- "$output_tmp/output" "$destination"
