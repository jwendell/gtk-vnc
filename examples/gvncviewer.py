#!/usr/bin/python

import gtk
import gtkvnc
import sys

if len(sys.argv) != 2 and len(sys.argv) != 3:
    print "syntax: gvncviewer.py host:display [password]"
    sys.exit(1)

def set_title(vnc, window, grabbed):
    name = vnc.get_name()
    if grabbed:
        subtitle = "(Press Ctrl+Alt to release pointer) "
    else:
        subtitle = ""

    window.set_title("%s%s - GVncViewer" % (subtitle, name))

def vnc_grab(src, window):
    set_title(src, window, True)

def vnc_ungrab(src, window):
    set_title(src, window, False)

def vnc_connected(src):
    print "Connected to server"

def vnc_initialized(src, window):
    print "Connection initialized"
    set_title(src, window, False)
    window.show_all()

def vnc_disconnected(src):
    print "Disconnected from server"
    gtk.main_quit()

def send_caf1(src, vnc):
    print "Send Ctrl+Alt+F1"
    vnc.send_keys(["Control_L", "Alt_L", "F1"])

def send_caf7(src, vnc):
    print "Send Ctrl+Alt+F7"
    vnc.send_keys(["Control_L", "Alt_L", "F7"])

def send_cad(src, vnc):
    print "Send Ctrl+Alt+Del"
    vnc.send_keys(["Control_L", "Alt_L", "Del"])

def send_cab(src, vnc):
    print "Send Ctrl+Alt+BackSpace"
    vnc.send_keys(["Control_L", "Alt_L", "BackSpace"])

window = gtk.Window()
vnc = gtkvnc.Display()

layout = gtk.VBox()
window.add(layout)

menubar = gtk.MenuBar()
sendkeys = gtk.MenuItem("_Send keys")
menubar.append(sendkeys)

buttons = gtk.HBox()
caf1 = gtk.MenuItem("Ctrl+Alt+F_1")
caf7 = gtk.MenuItem("Ctrl+Alt+F_7")
cad = gtk.MenuItem("Ctrl+Alt+_Del")
cab = gtk.MenuItem("Ctrl+Alt+_Backspace")

submenu = gtk.Menu()
submenu.append(caf1)
submenu.append(caf7)
submenu.append(cad)
submenu.append(cab)
sendkeys.set_submenu(submenu)

caf1.connect("activate", send_caf1, vnc)
caf7.connect("activate", send_caf7, vnc)
cad.connect("activate", send_cad, vnc)
cab.connect("activate", send_cab, vnc)


layout.add(menubar)
layout.add(vnc)

vnc.realize()
vnc.set_pointer_grab(True)
vnc.set_keyboard_grab(True)
#v.set_pointer_local(True)

if len(sys.argv) == 3:
    vnc.set_password(sys.argv[2])

disp = sys.argv[1].find(":")
if disp != -1:
    host = sys.argv[1][:disp]
    port = str(5900 + int(sys.argv[1][disp+1:]))
else:
    host = sys.argv[1]
    port = "5900"
print "Connecting to %s %s" % (host, port)

vnc.open_host(host, port)
vnc.connect("vnc-pointer-grab", vnc_grab, window)
vnc.connect("vnc-pointer-ungrab", vnc_ungrab, window)

vnc.connect("vnc-connected", vnc_connected)
vnc.connect("vnc-initialized", vnc_initialized, window)
vnc.connect("vnc-disconnected", vnc_disconnected)

gtk.main()
