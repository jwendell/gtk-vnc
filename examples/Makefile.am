
if WITH_EXAMPLES
bin_PROGRAMS = gvncviewer
else
noinst_PROGRAMS = gvncviewer
endif

gvncviewer_SOURCES = gvncviewer.c
gvncviewer_LDADD = ../src/libgtk-vnc-1.0.la @GTK_CFLAGS@ @GTKGLEXT_LIBS@ \
		   @VIEW_LIBS@
gvncviewer_CFLAGS = @GTK_CFLAGS@ @GTKGLEXT_CFLAGS@ @WARNING_CFLAGS@ \
		    @VIEW_CFLAGS@ -I$(top_srcdir)/src/

EXTRA_DIST = gvncviewer.py
