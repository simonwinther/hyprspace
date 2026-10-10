#!/usr/bin/env python3
"""Disposable Wayland windows with an input log for leak detection."""

import os
from pathlib import Path
import sys
import gi

gi.require_version("Gtk", "3.0")
from gi.repository import Gtk, GLib

if os.environ.get("HS_APP_ID"):
    GLib.set_prgname(os.environ["HS_APP_ID"])

windows = []
log = Path(os.environ.get("HS_INPUT_LOG", "/dev/null"))


def content_marker(widget, context):
    context.set_source_rgb(36 / 255, 229 / 255, 87 / 255)
    context.paint()


def event(window, event):
    with log.open("a") as output:
        output.write(
            f"{window.get_title()} {event.type} {getattr(event, 'keyval', '')}\n"
        )
    return False


for title in sys.argv[1:]:
    window = Gtk.Window(title=title)
    window.set_default_size(640, 400)
    if title == os.environ.get("HS_CONTENT_MARKER"):
        content = Gtk.DrawingArea()
        content.connect("draw", content_marker)
        window.add(content)
    else:
        window.add(Gtk.Label(label=title))
    window.connect("key-press-event", event)
    window.connect("key-release-event", event)
    window.connect("destroy", lambda w: Gtk.main_quit())
    window.show_all()
    windows.append(window)

if os.environ.get("HS_TITLE_FILE"):
    def update_title():
        path = Path(os.environ["HS_TITLE_FILE"])
        if path.exists():
            windows[0].set_title(path.read_text().strip())
        return True

    GLib.timeout_add(50, update_title)

Gtk.main()
