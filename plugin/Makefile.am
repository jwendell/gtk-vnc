if ENABLE_PLUGIN

# What we really want is 'noinst_LTLIBRARIES', so that it builds the
# shared library but lets us control how we install it.  However
# if you do that, automake thinks you're trying to build a "libtool
# convenience library" and no longer builds a shared library at all.
# Very unhelpful.  So instead build and install the library in libdir
# but delete it in the install hook below and install it where we want.
# I hate automake and libtool.
lib_LTLIBRARIES = libgtk-vnc-plugin.la

libgtk_vnc_plugin_la_SOURCES = \
	gtk-vnc-plugin.c gtk-vnc-plugin.h npshell.c npunix.c
libgtk_vnc_plugin_la_LIBADD = \
	../src/libgtk-vnc-1.0.la @GTK_LIBS@ @FIREFOX_PLUGIN_LIBS@
libgtk_vnc_plugin_la_CFLAGS = \
	-I$(top_srcdir)/src @GTK_CFLAGS@ @FIREFOX_PLUGIN_CFLAGS@ \
	@WARNING_CFLAGS@ @WERROR_CFLAGS@

all-local: gtk-vnc-plugin.so

gtk-vnc-plugin.so: libgtk-vnc-plugin.la
	cp .libs/libgtk-vnc-plugin.so.0.0.0 $@

install-exec-hook:
	rm -f $(libdir)/libgtk-vnc-plugin.*
	$(install_sh) -d $(libdir)/mozilla/plugins
	$(install_sh) -c -m 0755 gtk-vnc-plugin.so $(libdir)/mozilla/plugins

EXTRA_DIST = README

CLEANFILES = gtk-vnc-plugin.so

endif