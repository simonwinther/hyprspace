"""Exercise the compiled desktop slider against a disposable compositor."""

import argparse
import hashlib
import json
import subprocess
from pathlib import Path

from run import Suite, wait_for
from spaces import create, local, spawn, state, switch


def run(slider):
    s = Suite(group="space-bar")
    try:
        (s.root / "slider-binary.json").write_text(json.dumps({
            "path": str(slider), "sha256": hashlib.sha256(slider.read_bytes()).hexdigest()
        }, indent=2))

        def snapshot(index=0):
            return json.loads(subprocess.check_output(
                [str(slider), "--snapshot", s.names[index]], env=s.env, text=True, timeout=3))

        original = state(s)["active"]
        for index, number in [(0, 1), (1, 5), (2, 3)]:
            s.ctl("dispatch", "focusmonitor", s.names[index])
            spawn(s, wait_for, f"hs-bar-default-{number}", number)
        before = [snapshot(index) for index in range(3)]
        assert all(set(value["occupied"]) == {1, 3, 5} and value["space"] == original for value in before)
        s.check("compiled slider projects local occupancy and remote monitor ownership from the active space")

        process = s.spawn(["env", "WAYBAR_OUTPUT_NAME=" + s.names[0], str(slider)])
        layer = wait_for(lambda: s.layer("waybar-workspace-slider-" + s.names[0]))

        def click(number):
            s.move((layer["x"] + 25 + (number - 1) * 30, layer["y"] + 18))
            s.button(1)
            s.button(0)
            wait_for(lambda: snapshot()["active"] == number)
            assert process.poll() is None

        click(7)
        s.ctl("keyword", "general:workspace_back_and_forth", "true")
        click(7)
        assert snapshot()["space"] == original
        s.ctl("keyword", "general:workspace_back_and_forth", "false")
        original_seven = local(s, original, 7)["native_id"]
        s.check("real slider clicks select local slots and repeated active-slot clicks stay selected")

        hobby = create(s, wait_for, "Bar Hobby")
        empty = [snapshot(index) for index in range(3)]
        assert all(value["space"] == hobby and value["occupied"] == [] and value["remote"] == {} for value in empty)
        click(7)
        assert local(s, hobby, 7)["native_id"] != original_seven
        spawn(s, wait_for, "hs-bar-hobby-7", 7)
        wait_for(lambda: snapshot()["occupied"] == [7])
        assert all(snapshot(index)["occupied"] == [7] for index in range(3))
        s.check("an empty space clears every output's old occupancy; clicking the same local number creates an independent native workspace")

        s.scroll()
        def focused_local():
            return next(value["number"] for value in json.loads(s.request("spaces"))["monitors"] if value["focused"])
        wait_for(lambda: focused_local() != 7)
        assert snapshot()["space"] == hobby
        current = focused_local()
        assert local(s, hobby, current)
        s.check("real slider wheel input stays within its displayed space")

        switch(s, wait_for, original)
        assert all(set(snapshot(index)["occupied"]) == {1, 3, 5} for index in range(3))
        work = create(s, wait_for, "Bar Work")
        assert all(snapshot(index)["occupied"] == [] for index in range(3))
        (s.root / "slider-projections.json").write_text(json.dumps({
            "default": before, "empty_hobby": empty, "work": [snapshot(index) for index in range(3)]
        }, indent=2))
        s.check("switching back restores prior local occupancy and another empty space inherits none")

        s.ctl("plugin", "unload", str(s.plugin))
        unavailable = subprocess.run([str(slider), "--snapshot", s.names[0]], env=s.env,
                                     text=True, capture_output=True, timeout=3)
        assert unavailable.returncode == 1 and unavailable.stdout == ""
        s.ctl("plugin", "load", str(s.plugin))
        wait_for(lambda: snapshot()["space"] == work)
        assert snapshot()["occupied"] == [] and process.poll() is None
        s.check("plugin unavailability fails closed and reloading restores the displayed namespace")
    except BaseException:
        try:
            (s.root / "slider-failure.json").write_text(json.dumps({
                "spaces": json.loads(s.request("spaces")), "monitors": s.data("monitors"),
                "layers": s.data("layers")
            }, indent=2))
        except Exception:
            pass
        raise
    finally:
        s.finish()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--slider", required=True, type=Path)
    run(parser.parse_args().slider.resolve())
