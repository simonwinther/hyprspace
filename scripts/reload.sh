#!/usr/bin/env bash
# Stage and check before unloading; a failed unload must never be ignored.
set -euo pipefail

source_so=${1:?expected the built plugin path}
plugin_so=${2:?expected the installed plugin path}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
plugin_dir=$(dirname -- "$plugin_so")
[[ "$plugin_so" == /* ]] || { echo "error: plugin path must be absolute" >&2; exit 1; }

mkdir -p -- "$plugin_dir"
exec 9>"$plugin_dir/.reload.lock"
flock -n 9 || { echo "error: another plugin reload is in progress" >&2; exit 1; }
reload_tmp=$(mktemp -d -- "$plugin_dir/.hyprspace-reload.XXXXXX")
trap 'rm -f -- "$reload_tmp/new.so" "$reload_tmp/previous.so"; rmdir -- "$reload_tmp"' EXIT
trap 'exit 130' INT
trap 'exit 143' HUP TERM

install -m 0755 -- "$source_so" "$reload_tmp/new.so"
bash "$script_dir/check-abi.sh" "$reload_tmp/new.so" --require-running
if [[ -f "$plugin_so" ]]; then
    install -m 0755 -- "$plugin_so" "$reload_tmp/previous.so"
fi

plugin_loaded() {
    local plugins
    plugins=$(hyprctl -j plugin list) || return 2
    jq -e 'if type == "array" then any(.[]; .name == "hyprspace") else error("invalid plugin list") end' <<< "$plugins" >/dev/null
}

request() {
    local reply
    reply=$(hyprctl "$@") || { echo "error: hyprctl $* failed: $reply" >&2; return 1; }
    [[ "$reply" == ok ]] || { echo "error: hyprctl $* returned: $reply" >&2; return 1; }
}

was_loaded=false
if plugin_loaded; then
    was_loaded=true
    request plugin unload "$plugin_so"
    if plugin_loaded; then
        echo "error: hyprspace is still loaded; installation cancelled" >&2
        exit 1
    elif [[ $? -ne 1 ]]; then
        echo "error: cannot verify that hyprspace unloaded" >&2
        exit 1
    fi
elif [[ $? -ne 1 ]]; then
    echo "error: cannot read the loaded plugin list" >&2
    exit 1
fi

mv -fT -- "$reload_tmp/new.so" "$plugin_so"
if ! request plugin load "$plugin_so"; then
    # A load error may have left a plugin registered. Do not overwrite its
    # pathname or try to load a second copy until that state is understood.
    if plugin_loaded; then
        echo "error: load reported failure but hyprspace is registered; leaving the file in place" >&2
        exit 1
    elif [[ $? -ne 1 ]]; then
        echo "error: cannot verify plugin state after load failure; leaving the file in place" >&2
        exit 1
    fi
    if [[ -f "$reload_tmp/previous.so" ]]; then
        mv -fT -- "$reload_tmp/previous.so" "$plugin_so"
        echo "Restored the previous installed binary." >&2
        if "$was_loaded"; then
            bash "$script_dir/check-abi.sh" "$plugin_so" --require-running || { echo "error: previous binary could not be verified; leaving it unloaded" >&2; exit 1; }
            request plugin load "$plugin_so" || { echo "error: previous plugin could not be reloaded" >&2; exit 1; }
        fi
    fi
    exit 1
fi

request reload
echo "Reloaded $plugin_so"
