#!/usr/bin/python

import gtk
import gtkvnc


w = gtk.Window()

v = gtkvnc.Display()

w.add(v)
w.show_all()

v.set_credential(gtkvnc.CREDENTIAL_PASSWORD, "123456")
v.open_host("localhost", "5901", 0)

gtk.main()
