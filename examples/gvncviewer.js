#!/usr/bin/gjs

const Vnc = imports.gi.GtkVnc;
const Gtk = imports.gi.Gtk;

Gtk.init(0, null);

var win = new Gtk.Window({title: "GTK-VNC with JavaScript"});
var disp = new Vnc.Display();

win.add(disp);
disp.open_host("localhost", "5900");
win.show_all();
Gtk.main();