#!/usr/bin/env python3
"""Disposable Wayland windows with an input log for leak detection."""

import os
from pathlib import Path
import sys
import gi

gi.require_version("Gtk", "3.0")
from gi.repository import Gtk

windows = []
log = Path(os.environ.get("HS_INPUT_LOG", "/dev/null"))


def event(window, event):
    with log.open("a") as output:
        output.write(
            f"{window.get_title()} {event.type} {getattr(event, 'keyval', '')}\n"
        )
    return False


for title in sys.argv[1:]:
    window = Gtk.Window(title=title)
    window.set_default_size(640, 400)
    window.add(Gtk.Label(label=title))
    window.connect("key-press-event", event)
    window.connect("key-release-event", event)
    window.connect("destroy", lambda w: Gtk.main_quit())
    window.show_all()
    windows.append(window)
Gtk.main()
