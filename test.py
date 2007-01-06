import vnc, gtk, socket, sys

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])

v = vnc.Display()
v.show()
v.open(s.fileno())

win = gtk.Window()
win.add(v)
win.show_all()
win.set_title('QEMU')

def enter_grab(v, win):
    win.set_title('QEMU - Press Ctrl-Alt to exit grab')

def leave_grab(v, win):
    win.set_title('QEMU')

v.connect('enter-grab-event', enter_grab, win)
v.connect('leave-grab-event', leave_grab, win)

gtk.main()
