
EXTRA_DIST = libgtk-vnc_sym.version

lib_LTLIBRARIES = libgtk-vnc-1.0.la

libgtk_vnc_1_0_la_LIBADD = @GTK_LIBS@ @GNUTLS_LIBS@
libgtk_vnc_1_0_la_CFLAGS = @GTK_CFLAGS@ @GNUTLS_CFLAGS@ @WARNING_CFLAGS@
libgtk_vnc_1_0_la_LDFLAGS = -Wl,--version-script=$(srcdir)/libgtk-vnc_sym.version \
                            -version-info 0:1:0

gtk_vnc_includedir = $(includedir)/gtk-vnc-1.0/
gtk_vnc_include_HEADERS = vncdisplay.h gvnc.h coroutine.h continuation.h

libgtk_vnc_1_0_la_SOURCES = blt.h blt1.h \
	continuation.h continuation.c \
	coroutine.h coroutine.c \
	d3des.h d3des.c \
	gvnc.h gvnc.c \
	vncdisplay.h vncdisplay.c \
	vncshmimage.h vncshmimage.c

if WITH_PYTHON
python_LTLIBRARIES = gtkvnc.la

gtkvnc_la_LIBADD = libgtk-vnc-1.0.la @PYGTK_LIBS@
gtkvnc_la_CFLAGS = @GTK_CFLAGS@ @WARNING_CFLAGS@ @PYTHON_INCLUDES@ @PYGTK_CFLAGS@
gtkvnc_la_LDFLAGS = -module -avoid-version -fPIC
gtkvnc_la_SOURCES = vncmodule.c vncmodule.defs.c

CODEGENDIR = $(shell pkg-config --variable=codegendir pygtk-2.0)
DEFSDIR = $(shell pkg-config --variable=defsdir pygtk-2.0)

vnc.defs: vncdisplay.h
	$(PYTHON) $(CODEGENDIR)/h2def.py $< > $@

vncmodule.defs.c: vnc.override vnc.defs
	pygtk-codegen-2.0 --prefix gtkvnc \
                          --register $(DEFSDIR)/gdk-types.defs \
                          --register $(DEFSDIR)/gtk-types.defs \
                          --override $(srcdir)/vnc.override vnc.defs > $@

CLEANFILES = vnc.defs
EXTRA_DIST += vnc.override

else
EXTRA_DIST += vnc.override vncmodule.c
endif

