#!/usr/bin/env python3
"""Capture window grouping in a private compositor with disposable project content."""

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import threading
import time


def document(path):
    import gi
    gi.require_version("Gtk", "3.0")
    from gi.repository import GLib, Gtk

    GLib.set_prgname("org.gnome.TextEditor")
    window = Gtk.Window(title=path.stem)
    window.set_default_size(800, 600)
    header = Gtk.HeaderBar(title=path.stem, subtitle="Hyprspace experiment")
    header.set_show_close_button(True)
    window.set_titlebar(header)
    view = Gtk.TextView()
    view.set_editable(False)
    view.set_wrap_mode(Gtk.WrapMode.WORD)
    view.set_left_margin(32)
    view.set_right_margin(32)
    view.set_top_margin(24)
    view.set_bottom_margin(24)
    view.get_buffer().set_text(path.read_text())
    css = Gtk.CssProvider()
    css.load_from_data(b"textview { font: 18px Sans; } textview text { background: #f3f1ea; color: #28303c; }")
    view.get_style_context().add_provider(css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
    scroll = Gtk.ScrolledWindow()
    scroll.add(view)
    window.add(scroll)
    window.connect("destroy", lambda widget: Gtk.main_quit())
    window.show_all()
    Gtk.main()


def capture(destination):
    from run import REPO, Suite, wait_for

    destination.mkdir(parents=True, exist_ok=True)
    class DemoSuite(Suite):
        def start(self):
            applications = Path(self.env["XDG_DATA_HOME"]) / "applications"
            applications.mkdir(exist_ok=True)
            (applications / "org.gnome.TextEditor.desktop").write_text(
                "[Desktop Entry]\nType=Application\nName=Notes\nExec=true\nIcon=accessories-text-editor\nStartupWMClass=org.gnome.TextEditor\n")
            super().start()

    suite = DemoSuite(group="window-views-demo")
    recorder = None
    try:
        project = suite.root / "hyprspace"
        project.mkdir()
        for source in ("WindowBoard.cpp", "WindowViewModel.hpp", "OverviewSession.cpp"):
            shutil.copyfile(REPO / "src" / source, project / source)
        shutil.copyfile(REPO / "contrib/hyprspace.conf", project / "hyprspace.conf")
        notes = project / "Design notes.txt"
        notes.write_text("WINDOW VIEWS\n\nFind a window without remembering its workspace.\n\n"
                         "All windows\nA clear, readable board for a quick scan.\n\n"
                         "Apps\nBring browser windows, terminals and documents together.\n\n"
                         "Workspaces and monitors\nKeep the context when location matters.\n\n"
                         "Search\nTry a title, app name or workspace. Æ, Ø and Å work too.\n\n"
                         "Inspection\nHold Z, zoom into the preview, then release to return.\n")
        research = suite.root / "Research"
        research.mkdir()
        shutil.copyfile(REPO / "docs/interactive.md", research / "Interactive overview.md")
        shutil.copyfile(REPO / "docs/guide.md", research / "Controls.md")
        suite.ctl("keyword", "monitor", f"{suite.names[0]},1920x1080@60,0x0,1")
        suite.ctl("keyword", "animations:enabled", "true")
        suite.ctl("keyword", "general:gaps_in", "9")
        suite.ctl("keyword", "general:gaps_out", "18")

        def launch(workspace, output, command):
            suite.ctl("keyword", "workspace", f"{workspace},monitor:{suite.names[output]},persistent:true")
            suite.ctl("dispatch", "focusmonitor", suite.names[output])
            suite.ctl("dispatch", "workspace", str(workspace))
            before = {window["address"] for window in suite.data("clients")}
            suite.spawn(command)
            return wait_for(lambda: next((window for window in suite.data("clients") if window["address"] not in before), None), timeout=15)

        first = launch(31, 0, ["ghostty", "--config-default-files=false", "--font-size=14", "--background=151923",
                              "--title=Window board · hyprspace", "-e", "nvim", "-u", "NONE", "+set number", "+syntax on", str(project / "WindowBoard.cpp")])
        launch(32, 0, ["ghostty", "--config-default-files=false", "--font-size=14", "--background=151923",
                       "--title=Grouping model · hyprspace", "-e", "nvim", "-u", "NONE", "+set number", "+syntax on", str(project / "WindowViewModel.hpp")])
        launch(31, 0, ["nautilus", "--new-window", str(project)])
        launch(33, 1, ["nautilus", "--new-window", str(research)])
        launch(34, 2, ["python3", str(Path(__file__).resolve()), "document", str(notes)])
        second_notes = research / "Keyboard navigation.txt"
        second_notes.write_text("FIND YOUR WAY\n\nG opens the window board.\n\nShift+G changes grouping.\n\n"
                                "/ searches every title and app.\n\nR keeps recent windows at the front.\n\n"
                                "Arrows or Tab select. Enter opens.\n\nEscape clears search, returns to workspaces, then closes.\n")
        launch(33, 1, ["python3", str(Path(__file__).resolve()), "document", str(second_notes)])
        suite.ctl("dispatch", "focuswindow", "address:" + first["address"])
        suite.move((100, 140))
        suite.ctl("dispatch", "hyprspace:overview", "on")
        wait_for(lambda: suite.status()["live"])
        time.sleep(.8)
        suite.ctl("dismissnotify")
        suite.run("grim", "-s", "1", "-o", suite.names[0], str(destination / "window-views-workspaces.png"))

        log = (suite.root / "recorder.log").open("w")
        recorder = FrameRecorder(suite, destination / "window-views.mp4", log)
        for mode in ("flat", "app", "workspace", "monitor"):
            suite.ctl("dispatch", "hyprspace:windowview", mode)
            wait_for(lambda: suite.status()["window_board"]["grouping"] == mode)
            time.sleep(1.1)
            suite.run("grim", "-s", "1", "-o", suite.names[0], str(destination / f"window-views-{mode}.png"))
        suite.ctl("dispatch", "hyprspace:windowview", "app")
        suite.run("wtype", "-k", "slash", "grouping")
        wait_for(lambda: suite.status()["window_board"]["count"] == 1)
        time.sleep(1)
        suite.run("grim", "-s", "1", "-o", suite.names[0], str(destination / "window-views-search.png"))
        suite.run("wtype", "-k", "Escape")
        suite.key(44, 1)
        time.sleep(.8)
        suite.key(13, 1)
        suite.key(13, 0)
        time.sleep(.8)
        suite.key(44, 0)
        time.sleep(.8)
        suite.ctl("dispatch", "hyprspace:windowview", "off")
        time.sleep(1)
        recorder.finish()
        recorder = None
        (destination / "window-views-capture.json").write_text(json.dumps({
            "compositor": suite.compositor_identity,
            "plugin_sha256": suite.snapshot.record["files"]["hyprspace.so"]["sha256"],
            "outputs": suite.data("monitors"), "windows": len(suite.data("clients")),
            "content": "Copied project source in Ghostty/Neovim and Files; disposable GTK notes",
        }, indent=2))
        suite.check("captured all window groups, search and inspection with private demo content")
    finally:
        try:
            if recorder is not None:
                recorder.finish()
        finally:
            suite.finish()


class FrameRecorder:
    """Read only the private Wayland output, avoiding physical display capture."""

    def __init__(self, suite, path, log):
        self.suite = suite
        self.stop = threading.Event()
        self.error = None
        self.grim = None
        self.encoder = subprocess.Popen([
            "ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-f", "image2pipe",
            "-vcodec", "ppm", "-framerate", "15", "-i", "-", "-c:v", "libx264",
            "-threads", "2", "-preset", "ultrafast", "-crf", "20", "-pix_fmt", "yuv420p",
            "-movflags", "+faststart", str(path),
        ], stdin=subprocess.PIPE, stdout=log, stderr=log)
        self.thread = threading.Thread(target=self.record)
        self.thread.start()

    def record(self):
        epoch, written, previous = time.monotonic(), 0, None
        try:
            while not self.stop.is_set():
                start = time.monotonic()
                self.grim = subprocess.Popen(["grim", "-s", "1", "-t", "ppm", "-o", self.suite.names[0], "-"],
                                             env=self.suite.env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
                try:
                    frame = self.grim.communicate(timeout=2)[0]
                except subprocess.TimeoutExpired:
                    self.grim.kill()
                    self.grim.communicate()
                    continue
                if self.grim.returncode != 0:
                    if self.stop.is_set():
                        break
                    raise RuntimeError("private output capture failed")
                # Keep video time aligned with wall time if a capture waits for
                # a frame. The encoder never needs to outrun the compositor.
                expected = int((time.monotonic() - epoch) * 15)
                while previous is not None and written < expected:
                    self.encoder.stdin.write(previous)
                    written += 1
                self.encoder.stdin.write(frame)
                written += 1
                previous = frame
                self.stop.wait(max(0, 1 / 15 - (time.monotonic() - start)))
        except BaseException as error:
            self.error = error
        finally:
            self.encoder.stdin.close()

    def finish(self):
        self.stop.set()
        if self.grim is not None and self.grim.poll() is None:
            self.grim.terminate()
        self.thread.join(timeout=5)
        assert not self.thread.is_alive(), "private recorder did not stop"
        self.encoder.wait(timeout=30)
        assert self.error is None, self.error
        assert self.encoder.returncode == 0, self.encoder.returncode


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "document":
        document(Path(sys.argv[2]))
    else:
        parser = argparse.ArgumentParser(description=__doc__)
        parser.add_argument("--output", type=Path, default=Path("docs/screenshots"))
        capture(parser.parse_args().output.resolve())
