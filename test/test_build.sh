#!/usr/bin/env bash
# All hyprctl commands in this suite go to a fixture, never a live session.
set -euo pipefail
test_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(dirname -- "$test_dir")
test_tmp=$(mktemp -d)
trap 'rm -rf -- "$test_tmp"' EXIT
trap 'exit 130' INT
trap 'exit 143' HUP TERM

export HYPRSPACE_TEST_STATE="$test_tmp/state"
export HYPRSPACE_TEST_NM
HYPRSPACE_TEST_NM=$(command -v nm)
mkdir -p "$test_tmp/bin" "$HYPRSPACE_TEST_STATE" "$test_tmp/install with spaces"
for tool in hyprctl nm cxx; do
    install -m 0755 "$test_dir/fixtures/$tool" "$test_tmp/bin/$tool"
done
export PATH="$test_tmp/bin:$PATH"
export HYPRLAND_INSTANCE_SIGNATURE=hyprspace-test
# Force hyprctl's socket selection into the fixture namespace as well.
unset WAYLAND_DISPLAY

checks=0
check() {
    checks=$((checks + 1))
    if ! "$@"; then
        printf 'FAIL: %s\n' "$*" >&2
        exit 1
    fi
}

rejects() {
    local message=$1
    shift
    if "$@" > "$test_tmp/result" 2>&1; then
        echo "FAIL: unexpected success: $*" >&2
        return 1
    fi
    if ! rg -F -- "$message" "$test_tmp/result" >/dev/null; then
        sed -n '1,60p' "$test_tmp/result" >&2
        return 1
    fi
}

check_abi() {
    bash "$project_dir/scripts/check-abi.sh" "$@"
}

new_so="$HYPRSPACE_TEST_STATE/new.so"
old_so="$HYPRSPACE_TEST_STATE/old.so"
installed_so="$test_tmp/install with spaces/hyprspace.so"
${CXX:-g++} -shared -fPIC "$test_dir/fixtures/abi.cpp" -o "$old_so"
${CXX:-g++} -shared -fPIC -DTEST_REVISION=2 "$test_dir/fixtures/abi.cpp" -o "$new_so"

echo 'build: atomic replacement preserves open files and rejects partial output'
printf 'original\n' > "$installed_so"
exec {mapped_fd}< "$installed_so"
old_inode=$(stat -c %i "$installed_so")
check bash "$project_dir/scripts/atomic-output.sh" "$installed_so" install -m 0755 "$new_so"
check test "$(stat -c %i "$installed_so")" != "$old_inode"
IFS= read -r mapped_content <&"$mapped_fd"
check test "$mapped_content" = original
exec {mapped_fd}<&-
check cmp -s "$installed_so" "$new_so"
old_inode=$(stat -c %i "$installed_so")
check rejects 'fixture failure' bash "$project_dir/scripts/atomic-output.sh" "$installed_so" bash -c 'printf partial > "$1"; echo "fixture failure" >&2; exit 1' _
check test "$(stat -c %i "$installed_so")" = "$old_inode"
check cmp -s "$installed_so" "$new_so"
check rejects 'did not produce' bash "$project_dir/scripts/atomic-output.sh" "$installed_so" true

echo 'build: incremental builds track flags and system headers'
touch "$HYPRSPACE_TEST_STATE/system-header"
make_fixture() {
    make -s --no-print-directory -C "$project_dir" BUILD_DIR="$test_tmp/build" CXX="$test_tmp/bin/cxx" PKGS=cairo LDLIBS= SRCS=src/main.cpp "$@" all
}
check make_fixture CXXFLAGS=-O1
check test "$(wc -l < "$HYPRSPACE_TEST_STATE/compiler-calls")" = 2
old_inode=$(stat -c %i "$test_tmp/build/hyprspace.so")
check make_fixture CXXFLAGS=-O1
check test "$(wc -l < "$HYPRSPACE_TEST_STATE/compiler-calls")" = 2
check test "$(stat -c %i "$test_tmp/build/hyprspace.so")" = "$old_inode"
check make_fixture CXXFLAGS=-O2
check test "$(wc -l < "$HYPRSPACE_TEST_STATE/compiler-calls")" = 4
touch "$HYPRSPACE_TEST_STATE/system-header"
check make_fixture CXXFLAGS=-O2
check test "$(wc -l < "$HYPRSPACE_TEST_STATE/compiler-calls")" = 6
old_inode=$(stat -c %i "$test_tmp/build/hyprspace.so")
touch "$test_tmp/build/main.o"
export HYPRSPACE_TEST_LINK=fail
check rejects 'fixture linker failure' make_fixture CXXFLAGS=-O2
check test "$(stat -c %i "$test_tmp/build/hyprspace.so")" = "$old_inode"
unset HYPRSPACE_TEST_LINK

echo 'ABI: binary metadata, library compatibility and exact renderer exports'
check check_abi "$new_so"
export HYPRSPACE_TEST_VERSION=commit
check rejects 'ABI mismatch' check_abi "$new_so"
export HYPRSPACE_TEST_VERSION=library
check rejects 'ABI mismatch' check_abi "$new_so"
export HYPRSPACE_TEST_VERSION=malformed
check rejects 'cannot read the running Hyprland ABI' check_abi "$new_so"
export HYPRSPACE_TEST_VERSION=unavailable
check rejects 'cannot contact' check_abi "$new_so"
check env -u HYPRLAND_INSTANCE_SIGNATURE bash "$project_dir/scripts/check-abi.sh" "$new_so"
check rejects 'cannot contact' env -u HYPRLAND_INSTANCE_SIGNATURE bash "$project_dir/scripts/check-abi.sh" "$new_so" --require-running
unset HYPRSPACE_TEST_VERSION
export HYPRSPACE_TEST_SYMBOLS=missing
check rejects 'renderer symbol is not exported' check_abi "$new_so"
unset HYPRSPACE_TEST_SYMBOLS
objcopy --remove-section .hyprspace.abi "$new_so" "$test_tmp/no-metadata.so"
check rejects 'missing or invalid build ABI metadata' check_abi "$test_tmp/no-metadata.so"

reset_reload() {
    install -m 0755 "$old_so" "$installed_so"
    touch "$HYPRSPACE_TEST_STATE/loaded"
    : > "$HYPRSPACE_TEST_STATE/calls"
    unset HYPRSPACE_TEST_VERSION HYPRSPACE_TEST_UNLOAD HYPRSPACE_TEST_LOAD HYPRSPACE_TEST_LIST
}

reload_fixture() {
    bash "$project_dir/scripts/reload.sh" "$new_so" "$installed_so"
}

no_mutation() {
    ! rg '^(plugin (unload|load)|reload)' "$HYPRSPACE_TEST_STATE/calls" >/dev/null
}

echo 'reload: errors stop before replacement; failed loads restore the previous binary'
reset_reload
export HYPRSPACE_TEST_VERSION=commit
check rejects 'ABI mismatch' reload_fixture
check cmp -s "$installed_so" "$old_so"
check no_mutation

for failure in error response remains; do
    reset_reload
    export HYPRSPACE_TEST_UNLOAD=$failure
    check rejects 'error:' reload_fixture
    check cmp -s "$installed_so" "$old_so"
    check test -f "$HYPRSPACE_TEST_STATE/loaded"
    check test "$(rg -c '^plugin load' "$HYPRSPACE_TEST_STATE/calls" || true)" = ''
done

reset_reload
export HYPRSPACE_TEST_LIST=malformed
check rejects 'cannot read the loaded plugin list' reload_fixture
check cmp -s "$installed_so" "$old_so"
check no_mutation

reset_reload
export HYPRSPACE_TEST_LOAD=fail-new
check rejects 'Restored the previous installed binary' reload_fixture
check cmp -s "$installed_so" "$old_so"
check test -f "$HYPRSPACE_TEST_STATE/loaded"
check test "$(rg -c '^plugin load' "$HYPRSPACE_TEST_STATE/calls")" = 2

reset_reload
install -m 0755 "$test_tmp/no-metadata.so" "$installed_so"
export HYPRSPACE_TEST_LOAD=fail-new
check rejects 'previous binary could not be verified' reload_fixture
check cmp -s "$installed_so" "$test_tmp/no-metadata.so"
check test ! -f "$HYPRSPACE_TEST_STATE/loaded"
check test "$(rg -c '^plugin load' "$HYPRSPACE_TEST_STATE/calls")" = 1

reset_reload
exec 8>"$test_tmp/install with spaces/.reload.lock"
flock -n 8
check rejects 'another plugin reload is in progress' reload_fixture
check cmp -s "$installed_so" "$old_so"
check no_mutation
exec 8>&-

reset_reload
export HYPRSPACE_TEST_LOAD=registered-error
check rejects 'hyprspace is registered' reload_fixture
check cmp -s "$installed_so" "$new_so"
check test "$(rg -c '^plugin load' "$HYPRSPACE_TEST_STATE/calls")" = 1

reset_reload
check reload_fixture
check cmp -s "$installed_so" "$new_so"
check test -f "$HYPRSPACE_TEST_STATE/loaded"
check test "$(rg -c '^plugin unload' "$HYPRSPACE_TEST_STATE/calls")" = 1
check test "$(rg -c '^plugin load' "$HYPRSPACE_TEST_STATE/calls")" = 1
check test "$(rg -c '^reload$' "$HYPRSPACE_TEST_STATE/calls")" = 1

reset_reload
rm -f -- "$HYPRSPACE_TEST_STATE/loaded"
check reload_fixture
check cmp -s "$installed_so" "$new_so"
check test "$(rg -c '^plugin unload' "$HYPRSPACE_TEST_STATE/calls" || true)" = ''

printf '\n%d build and reload checks passed (no live compositor calls)\n' "$checks"
