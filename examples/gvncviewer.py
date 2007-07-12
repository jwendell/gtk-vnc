#!/usr/bin/python

import gtk
import gtkvnc
import sys

if len(sys.argv) != 3 and len(sys.argv) != 4:
    print "syntax: gvncviewer.py host port [password]"
    sys.exit(1)

w = gtk.Window()
v = gtkvnc.Display()

def vnc_grab(src):
    w.set_title("Press Ctrl+Alt to release pointer. GVncViewer")

def vnc_ungrab(src):
    w.set_title("GVncViewer")


w.add(v)
w.show_all()

v.set_pointer_grab(True)
v.set_keyboard_grab(True)
#v.set_pointer_local(True)

if len(sys.argv) == 4:
    v.set_password(sys.argv[3])
v.open_name(sys.argv[1], sys.argv[2])
v.connect("vnc-pointer-grab", vnc_grab)
v.connect("vnc-pointer-ungrab", vnc_ungrab)
v.connect("vnc-disconnected", gtk.main_quit)

gtk.main()
