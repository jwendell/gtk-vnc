
if WITH_EXAMPLES
bin_PROGRAMS = gvncviewer
else
noinst_PROGRAMS = gvncviewer
endif

gvncviewer_SOURCES = gvncviewer.c
gvncviewer_LDADD = ../src/libgtk-vnc-$(GTK_VNC_API_VERSION).la @GTK_CFLAGS@ \
		   @VIEW_LIBS@
gvncviewer_CFLAGS = @GTK_CFLAGS@ @WARNING_CFLAGS@ \
		    @VIEW_CFLAGS@ -I$(top_srcdir)/src/

EXTRA_DIST = gvncviewer.py gvncviewer.js

-include $(top_srcdir)/git.mk
