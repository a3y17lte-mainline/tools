#!/usr/bin/env python3
# usage: grad.py <width> <height> [css-gradient]   show one CSS gradient box of that size for 6 s
import sys, gi
gi.require_version("Gtk", "4.0")
from gi.repository import Gtk, Gdk, GLib

w, h = int(sys.argv[1]), int(sys.argv[2])
grad = sys.argv[3] if len(sys.argv) > 3 else "linear-gradient(to right, red, yellow, blue)"
css = Gtk.CssProvider()
css.load_from_string(".g { background: %s; }" % grad)

def activate(app):
    Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), css, 800)
    win = Gtk.ApplicationWindow(application=app)
    box = Gtk.Box()
    box.add_css_class("g")
    box.set_size_request(w, h)
    box.set_halign(Gtk.Align.CENTER)
    box.set_valign(Gtk.Align.CENTER)
    win.set_child(box)
    win.present()
    def done():
        print("scale=%d box=%dx%d window=%dx%d" % (win.get_scale_factor(), box.get_width(), box.get_height(), win.get_width(), win.get_height()), flush=True)
        app.quit()
    GLib.timeout_add_seconds(6, done)

app = Gtk.Application(application_id="org.example.gradtest")
app.connect("activate", activate)
app.run([])
