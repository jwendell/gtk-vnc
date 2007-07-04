#!/usr/bin/python

import gtk
import gtkvnc


w = gtk.Window()

v = gtkvnc.Display()

w.add(v)
w.show_all()

v.set_password("123456")
v.open_name("localhost", "5901")
v.connect("vnc-disconnected", gtk.main_quit)

gtk.main()
