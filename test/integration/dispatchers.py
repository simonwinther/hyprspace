"""Dispatcher ownership across real shared-library unloads."""

from pathlib import Path


def lifecycle(s):
    fixture = s.artifact("test-dispatchers.so")
    assert fixture.is_file(), "build integration-fixtures before the dispatcher suite"

    def load(path):
        s.ctl("plugin", "load", str(path))
        assert str(path) in Path(f"/proc/{s.compositor_pid}/maps").read_text()

    def unload(path):
        s.ctl("plugin", "unload", str(path))

    def reply(name, expected):
        result = s.run("hyprctl", "dispatch", name)
        assert expected in result, (name, result)
        assert s.compositor.poll() is None, "dispatcher invocation crashed the compositor"
        assert s.data("monitors"), "compositor IPC stopped responding"

    def unmapped(path):
        # A dependency or GNU-unique symbol must not accidentally keep the
        # provider mapped and turn an unload regression into a false pass.
        assert str(path) not in Path(f"/proc/{s.compositor_pid}/maps").read_text()

    s.close()
    unload(s.plugin)
    load(fixture)
    load(s.plugin)
    reply("hyprspace-test:ping", "fixture:original")
    reply("hyprspace-test:legacy", "ok")
    for name in ("raw", "closure"):
        reply("hyprspace-test:" + name, "fixture:raw")
    unload(fixture)
    unmapped(fixture)
    unload(s.plugin)
    reply("hyprspace-test:ping", "Invalid dispatcher")
    for name in ("legacy", "raw", "closure"):
        reply("hyprspace-test:" + name, "Invalid dispatcher")
    s.check("B loads before hyprspace: unloading B then hyprspace never resurrects B's callback")

    load(s.plugin)
    load(fixture)
    s.ctl("dispatch", "hyprspace-test:replace", "replacement")
    reply("submap", "fixture:replacement")
    unload(s.plugin)
    unmapped(s.plugin)
    reply("submap", "fixture:replacement")
    reply("hyprspace-test:ping", "fixture:original")
    s.check("a handler installed after hyprspace survives hyprspace unloading")
    unload(fixture)
    unmapped(fixture)
    reply("submap", "Invalid dispatcher")
    reply("hyprspace-test:ping", "Invalid dispatcher")
    s.check("hyprspace then B unload leaves no handler pointing into either library")

    load(s.plugin)
    load(fixture)
    for value in ("first", "second"):
        s.ctl("dispatch", "hyprspace-test:replace", value)
        reply("submap", "fixture:" + value)
    unload(fixture)
    unmapped(fixture)
    unload(s.plugin)
    reply("submap", "Invalid dispatcher")
    s.check("same-type handler replacement while both plugins are loaded cannot restore a removed registration")

    # Also cover a native name already replaced before hyprspace installs.
    load(fixture)
    s.ctl("dispatch", "hyprspace-test:replace", "early")
    load(s.plugin)
    reply("submap", "fixture:early")
    unload(fixture)
    unmapped(fixture)
    unload(s.plugin)
    reply("submap", "Invalid dispatcher")
    s.check("a foreign replacement present at install is never retained across provider unload")

    # submap was deliberately removed by B; check a native dispatcher that
    # was only wrapped by hyprspace, then leave the plugin ready for other suites.
    s.ctl("dispatch", "workspace", "11")
    assert s.data("activeworkspace")["id"] == 11
    s.check("untouched native dispatchers retain their behavior after unloading")
    load(s.plugin)
