
EXTRA_DIST = libgtk-vnc_sym.version vncmarshal.txt

lib_LTLIBRARIES = libgtk-vnc-1.0.la

libgtk_vnc_1_0_la_LIBADD = @GTK_LIBS@ @GNUTLS_LIBS@ \
			   @GTHREAD_LIBS@
libgtk_vnc_1_0_la_CFLAGS = @GTK_CFLAGS@ @GNUTLS_CFLAGS@ \
			   @GTHREAD_CFLAGS@ @WARNING_CFLAGS@ \
			   -DSYSCONFDIR=\""$(sysconfdir)"\" \
                           -DG_LOG_DOMAIN=\"gtk-vnc\"
libgtk_vnc_1_0_la_LDFLAGS = -Wl, @LD_VERSION_SCRIPT_SUPPORT@ \
                            -version-info 0:1:0

gtk_vnc_includedir = $(includedir)/gtk-vnc-1.0/
gtk_vnc_include_HEADERS = vncdisplay.h

libgtk_vnc_1_0_la_SOURCES = blt.h blt1.h \
	coroutine.h \
	d3des.h d3des.c \
	gvnc.h gvnc.c \
	vncdisplay.h vncdisplay.c \
        vncmarshal.h vncmarshal.c \
	x_keymap.h x_keymap.c vnc_keycodes.h \
	utils.h

if WITH_UCONTEXT
libgtk_vnc_1_0_la_SOURCES += continuation.h continuation.c coroutine_ucontext.c
EXTRA_DIST += coroutine_gthread.c
else
libgtk_vnc_1_0_la_SOURCES += coroutine_gthread.c
EXTRA_DIST += continuation.h continuation.c coroutine_ucontext.c
endif

vncmarshal.c: vncmarshal.txt
	glib-genmarshal --body $< > $@ || (rm -f $@ && exit 1)

vncmarshal.h: vncmarshal.txt
	glib-genmarshal --header $< > $@ || (rm -f $@ && exit 1)

vncdisplay.c:: vncmarshal.h

CLEANFILES = vncmarshal.c vncmarshal.h

if WITH_PYTHON
pyexec_LTLIBRARIES = gtkvnc.la

gtkvnc_la_LIBADD = libgtk-vnc-1.0.la @PYGTK_LIBS@
# Auto-generated C code for Python binding is full of compiler warnings :-(
#gtkvnc_la_CFLAGS = @GTK_CFLAGS@ @WARNING_CFLAGS@ @PYTHON_INCLUDES@ @PYGTK_CFLAGS@
gtkvnc_la_CFLAGS = @GTK_CFLAGS@ @PYTHON_INCLUDES@ \
		   @PYGTK_CFLAGS@
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

CLEANFILES += vnc.defs vncmodule.defs.c
EXTRA_DIST += vnc.override

else
EXTRA_DIST += vnc.override vncmodule.c
endif

