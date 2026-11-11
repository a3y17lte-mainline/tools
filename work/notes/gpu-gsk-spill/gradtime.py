#!/usr/bin/env python3
# usage: gradtime.py <width> <height> [css-background]
# Redraw one CSS box of that size every frame for 6 s and print the mean frame interval.
import sys, gi
gi.require_version("Gtk", "4.0")
from gi.repository import Gtk, Gdk, GLib

w, h = int(sys.argv[1]), int(sys.argv[2])
grad = sys.argv[3] if len(sys.argv) > 3 else "linear-gradient(to right, red, yellow, blue)"
css = Gtk.CssProvider()
css.load_from_string(".g { background: %s; }" % grad)
times = []

def tick(widget, clock):
    times.append(clock.get_frame_time())
    # change the content so that GSK cannot reuse the previous frame
    widget.set_opacity(0.99 if len(times) % 2 else 1.0)
    widget.queue_draw()
    return True

def activate(app):
    Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), css, 800)
    win = Gtk.ApplicationWindow(application=app)
    box = Gtk.Box()
    box.add_css_class("g")
    box.set_size_request(w, h)
    box.set_halign(Gtk.Align.CENTER)
    box.set_valign(Gtk.Align.CENTER)
    box.add_tick_callback(tick)
    win.set_child(box)
    win.present()
    def done():
        t = times[5:]
        ms = (t[-1] - t[0]) / 1000.0 / (len(t) - 1) if len(t) > 1 else -1
        print("scale=%d box=%dx%d window=%dx%d frames=%d ms/frame=%.1f" % (
            win.get_scale_factor(), box.get_width(), box.get_height(),
            win.get_width(), win.get_height(), len(t), ms), flush=True)
        app.quit()
    GLib.timeout_add_seconds(6, done)

app = Gtk.Application(application_id="org.example.gradtime")
app.connect("activate", activate)
app.run([])
