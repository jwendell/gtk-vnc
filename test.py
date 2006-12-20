import vnc, gtk, socket, sys

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])

v = vnc.Display()
v.show()
v.open(s.fileno())

win = gtk.Window()
win.add(v)
win.show_all()

gtk.main()
