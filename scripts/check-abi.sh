#!/usr/bin/env bash
# Verify a built hyprspace.so matches the Hyprland it will be loaded into.
#
# Hyprland's plugin ABI is tied to the exact commit hash of the compositor plus
# the versions of its hypr* libraries. Loading a mismatched plugin crashes the
# session, so this runs before install.

set -euo pipefail

SO="${1:-build/hyprspace.so}"

if [[ ! -f "$SO" ]]; then
    echo "error: $SO not found — run 'make' first" >&2
    exit 1
fi

fail() {
    echo "error: $*" >&2
    exit 1
}

# The hash the plugin was compiled against, taken from the installed headers.
HEADER_VERSION=""
for inc in $(pkg-config --cflags-only-I hyprland 2>/dev/null | tr ' ' '\n' | sed 's/^-I//') /usr/include; do
    [[ -n "$inc" ]] || continue
    for cand in "$inc/hyprland/src/version.h" "$inc/src/version.h" "$inc/version.h"; do
        if [[ -f "$cand" ]]; then HEADER_VERSION="$cand"; break 2; fi
    done
done
[[ -n "$HEADER_VERSION" ]] || fail "hyprland headers not found (install the 'hyprland' package)"

BUILT_HASH="$(sed -n 's/^#define GIT_COMMIT_HASH *"\(.*\)"/\1/p' "$HEADER_VERSION")"
[[ -n "$BUILT_HASH" ]] || fail "could not read GIT_COMMIT_HASH from $HEADER_VERSION"

# The hash of the Hyprland that is actually running.
if ! command -v hyprctl >/dev/null 2>&1; then
    echo "warning: hyprctl not found, skipping runtime ABI check"
    exit 0
fi

if ! RUNNING_RAW="$(hyprctl version 2>/dev/null)"; then
    echo "warning: Hyprland is not running, skipping runtime ABI check"
    echo "         plugin was built for commit $BUILT_HASH"
    exit 0
fi

RUNNING_HASH="$(printf '%s\n' "$RUNNING_RAW" | sed -n 's/.*at commit \([0-9a-f]\{40\}\).*/\1/p' | head -1)"

if [[ -z "$RUNNING_HASH" ]]; then
    echo "warning: could not parse the running Hyprland commit, skipping ABI check"
    exit 0
fi

if [[ "$BUILT_HASH" != "$RUNNING_HASH" ]]; then
    fail "ABI mismatch
  plugin built against: $BUILT_HASH
  Hyprland running:     $RUNNING_HASH
Update the hyprland package and headers to the same version, then 'make clean && make'."
fi

# Every renderer symbol the plugin borrows must actually be exported by the
# compositor binary — including renderWindow/getBackground, which are protected
# in the headers and reached via an explicit-instantiation accessor.
HYPRLAND_BIN="$(command -v Hyprland || true)"
if [[ -n "$HYPRLAND_BIN" ]] && command -v nm >/dev/null 2>&1; then
    # Materialise both symbol lists first: piping nm into `grep -q` would make
    # grep exit early, and under `pipefail` nm's SIGPIPE reads as a failure.
    EXPORTED="$(nm -D "$HYPRLAND_BIN" 2>/dev/null || true)"
    NEEDED="$(nm -D --undefined-only "$SO" | grep -oE '_ZN6Render13IHyprRenderer[A-Za-z0-9_]+' | sort -u || true)"

    MISSING=""
    while read -r sym; do
        [[ -n "$sym" ]] || continue
        case "$EXPORTED" in
            *" $sym"*) ;;
            *) MISSING="$MISSING $sym" ;;
        esac
    done <<<"$NEEDED"

    if [[ -n "$MISSING" ]]; then
        fail "these renderer symbols are not exported by $HYPRLAND_BIN:$MISSING"
    fi
fi

echo "ABI OK — built for Hyprland $BUILT_HASH, which is what is running"
