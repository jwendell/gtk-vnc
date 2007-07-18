#!/usr/bin/python

import gtk
import gtkvnc
import sys

if len(sys.argv) != 3 and len(sys.argv) != 4:
    print "syntax: gvncviewer.py host port [password]"
    sys.exit(1)

window = gtk.Window()
viewer = gtkvnc.Display()

def vnc_grab(src):
    window.set_title("Press Ctrl+Alt to release pointer. GVncViewer")

def vnc_ungrab(src):
    window.set_title("GVncViewer")

def send_caf1(src):
    print "Send Ctrl+Alt+F1"
    viewer.send_keys(["Control_L", "Alt_L", "F1"])
def send_caf7(src):
    print "Send Ctrl+Alt+F7"
    viewer.send_keys(["Control_L", "Alt_L", "F7"])
def send_cad(src):
    print "Send Ctrl+Alt+Del"
    print dir(viewer)
    viewer.send_keys(["Control_L", "Alt_L", "Del"])

layout = gtk.VBox()
window.add(layout)

buttons = gtk.HBox()
caf1 = gtk.Button("Ctrl+Alt+F1")
caf7 = gtk.Button("Ctrl+Alt+F7")
cad = gtk.Button("Ctrl+Alt+Del")
buttons.add(caf1)
buttons.add(caf7)
buttons.add(cad)
caf1.connect("clicked", send_caf1)
caf7.connect("clicked", send_caf7)
cad.connect("clicked", send_cad)


layout.add(buttons)
layout.add(viewer)

window.show_all()

viewer.set_pointer_grab(True)
viewer.set_keyboard_grab(True)
#v.set_pointer_local(True)

if len(sys.argv) == 4:
    viewer.set_password(sys.argv[3])
print "Connecting to %s %s" % (sys.argv[1], sys.argv[2])
viewer.open_name(sys.argv[1], sys.argv[2])
viewer.connect("vnc-pointer-grab", vnc_grab)
viewer.connect("vnc-pointer-ungrab", vnc_ungrab)
viewer.connect("vnc-disconnected", gtk.main_quit)

gtk.main()
