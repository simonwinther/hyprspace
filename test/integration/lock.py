"""Lock surfaces retain input across overlay activation and teardown."""

import json
from protocol import reply
import subprocess


def lifecycle(s, wait_for):
    def request(process, command):
        process.stdin.write(command.encode() + b"\n")
        process.stdin.flush()
        return json.loads(reply(process))

    def released():
        state = s.status()
        assert not state["live"] and not state["views"], state
        assert not state["keyboard_owned"] and not state["cursor_owned"], state
        assert not state["dragging"]
        assert all(window["alpha"] == 1 for window in state["windows"])

    def input_reaches_locker(process):
        s.move((400, 300))
        before = request(process, "counts")
        s.key(30, 1)
        s.key(30, 0)
        s.button(1)
        s.button(0)
        after = request(process, "counts")
        assert after["locked"]
        assert after["keys"] == before["keys"] + 2, (before, after)
        assert after["buttons"] == before["buttons"] + 2, (before, after)

    for initial in (None, "overview", "switch"):
        s.setup("dwindle", 0, 0)
        locker = s.spawn(
            [str(s.artifact("test-lock"))],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            bufsize=0,
        )
        assert reply(locker) == "ready"
        if initial:
            if initial == "switch":
                s.key(56, 1)
            s.ctl("dispatch", f"hyprspace:{initial}")
            wait_for(lambda: s.status()["cursor_owned"])
        request(locker, "lock")
        wait_for(lambda: request(locker, "counts")["locked"])
        if initial == "switch":
            s.key(56, 0)
        released()
        input_reaches_locker(locker)
        if initial is None:
            s.ctl("plugin", "unload", str(s.plugin))
            s.ctl("plugin", "load", str(s.plugin))
            released()
            input_reaches_locker(locker)
            s.check("loading the plugin into a locked session preserves lock-client input")
        if initial:
            s.check(f"locking an open {initial} restores visibility and releases keyboard/pointer ownership")
        for overlay in ("overview", "switch"):
            result = s.run("hyprctl", "dispatch", f"hyprspace:{overlay}")
            assert "locked" in result.lower(), result
            released()
            input_reaches_locker(locker)
            s.check(f"locked {initial or 'idle'} session rejects {overlay} and retains lock-client input")
        s.ctl("dispatch", "hyprspace:overview", "off")
        s.ctl("dispatch", "hyprspace:close")
        released()
        request(locker, "unlock")
        for overlay in ("overview", "switch"):
            s.ctl("dispatch", f"hyprspace:{overlay}")
            wait_for(lambda: s.status()["cursor_owned"])
            s.close()
            wait_for(lambda: not s.status()["cursor_owned"])
            released()
        s.check(f"unlocking the {initial or 'idle'} session restores normal overview and switcher activation")
        locker.terminate()
        locker.wait(timeout=3)
