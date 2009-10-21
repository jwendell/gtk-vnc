if ENABLE_PLUGIN

plugindir	= $(libdir)/mozilla/plugins
plugin_LTLIBRARIES = gtk-vnc-plugin.la

gtk_vnc_plugin_la_SOURCES = \
	gtk-vnc-plugin.c gtk-vnc-plugin.h npshell.c npunix.c
gtk_vnc_plugin_la_LIBADD = \
	../src/libgtk-vnc-1.0.la @GTK_LIBS@ \
	@FIREFOX_PLUGIN_LIBS@
gtk_vnc_plugin_la_LDFLAGS = \
	-module -avoid-version
gtk_vnc_plugin_la_CFLAGS = \
	-I$(top_srcdir)/src @GTK_CFLAGS@ \
	@FIREFOX_PLUGIN_CFLAGS@ @WARNING_CFLAGS@

all-local: gtk-vnc-plugin.so

gtk-vnc-plugin.so: gtk-vnc-plugin.la
	cp .libs/gtk-vnc-plugin.so $@

# Only leave the .so file in the plugins directory.
install-data-hook:
	rm -f $(DESTDIR)$(plugindir)/gtk-vnc-plugin.a $(DESTDIR)$(plugindir)/gtk-vnc-plugin.la

EXTRA_DIST = README

CLEANFILES = gtk-vnc-plugin.so

endif

-include $(top_srcdir)/git.mk
