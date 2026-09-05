#!/usr/bin/env python3
"""Foreground layer fixture with native keyboard and pointer handling."""

import os
from pathlib import Path
import sys
import gi

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
gi.require_version("GtkLayerShell", "0.1")
from gi.repository import Gdk, Gtk, GtkLayerShell

window = Gtk.Window()
GtkLayerShell.init_for_window(window)
GtkLayerShell.set_namespace(
    window, sys.argv[2] if len(sys.argv) > 2 else "hs-foreground"
)
GtkLayerShell.set_monitor(window, Gdk.Display.get_default().get_monitor(0))
GtkLayerShell.set_layer(window, GtkLayerShell.Layer.OVERLAY)
GtkLayerShell.set_keyboard_mode(window, GtkLayerShell.KeyboardMode.EXCLUSIVE)
GtkLayerShell.set_anchor(window, GtkLayerShell.Edge.TOP, True)
GtkLayerShell.set_anchor(window, GtkLayerShell.Edge.LEFT, True)
GtkLayerShell.set_margin(window, GtkLayerShell.Edge.TOP, 30)
GtkLayerShell.set_margin(window, GtkLayerShell.Edge.LEFT, 30)
window.set_default_size(260, 160)
css = Gtk.CssProvider()
css.load_from_data(b"window { background: #e04080; }")
window.get_style_context().add_provider(css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
entry = Gtk.Entry()
entry.set_margin_top(45)
entry.set_margin_bottom(45)
entry.set_margin_start(15)
entry.set_margin_end(15)
entry.connect("changed", lambda widget: Path(sys.argv[1]).write_text(widget.get_text()))
window.add(entry)
entry.connect(
    "button-press-event",
    lambda widget, event: Path(sys.argv[1] + ".click").write_text("clicked") and False,
)
if len(sys.argv) > 3 and sys.argv[3] == "bottom":
    GtkLayerShell.set_layer(window, GtkLayerShell.Layer.BOTTOM)
    GtkLayerShell.set_keyboard_mode(window, GtkLayerShell.KeyboardMode.NONE)
    GtkLayerShell.set_anchor(window, GtkLayerShell.Edge.RIGHT, True)
    GtkLayerShell.auto_exclusive_zone_enable(window)


def key(widget, event):
    if event.keyval == Gdk.KEY_Escape:
        Gtk.main_quit()
        return True
    return False


window.connect("key-press-event", key)
window.connect("destroy", lambda widget: Gtk.main_quit())
window.show_all()
entry.grab_focus()
Gtk.main()
