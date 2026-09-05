#!/usr/bin/env bash
# Inspect the binary without dlopen: even loading a mismatched plugin to ask
# for its version can run incompatible static constructors.
set -euo pipefail

SO=${1:-build/hyprspace.so}
REQUIRE_RUNNING=${2:-}

fail() {
    echo "error: $*" >&2
    exit 1
}

[[ -f "$SO" ]] || fail "$SO not found — run 'make' first"
for tool in readelf nm jq; do
    command -v "$tool" >/dev/null 2>&1 || fail "$tool is required (install binutils and jq)"
done

# This record was compiled into this exact file, not inferred from whatever
# headers happen to be installed now. Old binaries without it must be rebuilt.
METADATA=$(LC_ALL=C readelf --string-dump=.hyprspace.abi "$SO" 2>/dev/null) || fail "cannot read ABI metadata from $SO"
BUILT=$(sed -n 's/^.*\] *\([0-9a-f]\{40\}_aq_[^[:space:]]*\).*$/\1/p' <<< "$METADATA")
if [[ ! "$BUILT" =~ ^([0-9a-f]{40})_aq_([^_[:space:]]+)_hu_([^_[:space:]]+)_hg_([^_[:space:]]+)_hc_([^_[:space:]]+)_hlg_([^_[:space:]]+)$ ]]; then
    fail "missing or invalid build ABI metadata in $SO — rebuild the plugin"
fi

BUILT_HASH=${BASH_REMATCH[1]}
BUILT_VERSIONS=("${BASH_REMATCH[@]:2}")
BUILT_ABI=$BUILT_HASH
LABELS=(aq hu hg hc hlg)
for i in "${!LABELS[@]}"; do
    # Match __hyprland_api_get_client_hash(): drop only the final component.
    BUILT_ABI+="_${LABELS[i]}_${BUILT_VERSIONS[i]%.*}"
done

if ! RUNNING_RAW=$(hyprctl -j version 2>/dev/null); then
    if [[ "$REQUIRE_RUNNING" == --require-running || -n "${HYPRLAND_INSTANCE_SIGNATURE:-}" ]]; then
        fail "cannot contact the running Hyprland; refusing an unverified install"
    fi
    echo "warning: no running Hyprland; runtime ABI check skipped (built for $BUILT_ABI)"
    exit 0
fi

RUNNING_ABI=$(jq -er '.abiHash | strings | select(length > 0)' <<< "$RUNNING_RAW") || fail "cannot read the running Hyprland ABI"
[[ "$BUILT_ABI" == "$RUNNING_ABI" ]] || fail "ABI mismatch
  plugin built against: $BUILT_ABI
  Hyprland running:     $RUNNING_ABI
Use matching Hyprland headers and libraries, then rebuild the plugin."

# Inspect the executable of the selected session, which can differ from the
# installed binary after a package update. Never silently check the wrong one.
INSTANCES=$(hyprctl -j instances 2>/dev/null) || fail "cannot identify the running Hyprland executable"
RUNNING_PID=$(jq -er --arg instance "${HYPRLAND_INSTANCE_SIGNATURE:-}" '
    if $instance == "" then . else map(select(.instance == $instance)) end
    | select(length == 1) | .[0].pid | numbers | select(. > 0) | floor
' <<< "$INSTANCES") || fail "cannot identify a unique Hyprland session"
HYPRLAND_BIN="/proc/$RUNNING_PID/exe"

EXPORTED=$(nm -D --defined-only "$HYPRLAND_BIN" 2>/dev/null | awk '{print $NF}' | sort -u) || fail "cannot inspect $HYPRLAND_BIN"
NEEDED=$(nm -D --undefined-only "$SO" | awk '$NF ~ /^_ZN6Render13IHyprRenderer/ {print $NF}' | sort -u) || fail "cannot inspect symbols in $SO"
while IFS= read -r sym; do
    [[ -n "$sym" ]] || continue
    if ! awk -v wanted="$sym" '$0 == wanted { found = 1 } END { exit !found }' <<< "$EXPORTED"; then
        fail "renderer symbol is not exported by the running Hyprland: $sym"
    fi
done <<< "$NEEDED"

echo "ABI OK — $SO matches the running Hyprland ($BUILT_ABI)"
