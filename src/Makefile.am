
EXTRA_DIST = libgtk-vnc_sym.version vncmarshal.txt

lib_LTLIBRARIES = libgtk-vnc-1.0.la

libgtk_vnc_1_0_la_LIBADD = @GTK_LIBS@ @GNUTLS_LIBS@ \
			   @GTHREAD_LIBS@ @SASL_LIBS@ \
			   @X11_LIBS@ \
                           ../gnulib/lib/libgnu.la
libgtk_vnc_1_0_la_CFLAGS = @GTK_CFLAGS@ @GNUTLS_CFLAGS@ \
			   @GTHREAD_CFLAGS@ @SASL_CFLAGS@ @WARNING_CFLAGS@ \
			   @X11_CFLAGS@ \
			   -DSYSCONFDIR=\""$(sysconfdir)"\" \
			   -DPACKAGE_LOCALE_DIR=\""$(datadir)/locale"\" \
                           -DG_LOG_DOMAIN=\"gtk-vnc\" \
                           -I$(top_srcdir)/gnulib/lib -I../gnulib/lib
libgtk_vnc_1_0_la_LDFLAGS = -Wl,@LD_VERSION_SCRIPT_SUPPORT@ \
                            -version-info 0:1:0 -Wl,--no-undefined

gtk_vnc_includedir = $(includedir)/gtk-vnc-1.0/
gtk_vnc_include_HEADERS = vncdisplay.h

libgtk_vnc_1_0_la_SOURCES = blt.h blt1.h \
	coroutine.h \
	d3des.h d3des.c \
	dh.h dh.c \
	vncpixelformat.h \
	vncconnection.h vncconnection.c \
	vncdisplay.h vncdisplay.c \
        vncmarshal.h vncmarshal.c \
	x_keymap.h x_keymap.c vnc_keycodes.h \
        enums.h enums.c \
	utils.h

if WITH_UCONTEXT
libgtk_vnc_1_0_la_SOURCES += continuation.h continuation.c coroutine_ucontext.c
EXTRA_DIST += coroutine_gthread.c
else
libgtk_vnc_1_0_la_SOURCES += coroutine_gthread.c
EXTRA_DIST += continuation.h continuation.c coroutine_ucontext.c
endif

enum_headers = \
	$(top_srcdir)/src/vncdisplay.h

enums.c: @REBUILD@ $(enum_headers)
	glib-mkenums    --fhead "#include <glib-object.h>\n" \
			--fhead "#include \"enums.h\"\n\n" \
			--fprod "\n/* enumerations from \"@filename@\" */" \
			--fprod "\n#include \"@filename@\"\n" \
			--vhead "static const G@Type@Value _@enum_name@_values[] = {" \
			--vprod "  { @VALUENAME@, \"@VALUENAME@\", \"@valuenick@\" }," \
			--vtail "  { 0, NULL, NULL }\n};\n\n" \
			--vtail "GType\n@enum_name@_get_type (void)\n{\n" \
			--vtail "  static GType type = 0;\n\n" \
			--vtail "  if (!type)\n" \
			--vtail "    type = g_@type@_register_static (\"@EnumName@\", _@enum_name@_values);\n\n" \
			--vtail "  return type;\n}\n\n" \
		$(enum_headers) > $@

enums.h: @REBUILD@ $(enum_headers)
	glib-mkenums    --fhead "#ifndef __GTK_VNC_ENUMS_H__\n" \
			--fhead "#define __GTK_VNC_ENUMS_H__ 1\n\n" \
			--fhead "G_BEGIN_DECLS\n\n" \
			--ftail "G_END_DECLS\n\n" \
			--ftail "#endif /* __GTK_VNC_ENUMS_H__ */\n" \
			--fprod "\n/* --- @filename@ --- */" \
			--eprod "#define VNC_TYPE_@ENUMSHORT@ @enum_name@_get_type()\n" \
			--eprod "GType @enum_name@_get_type (void);\n" \
		$(enum_headers) >  $@

vncmarshal.c: vncmarshal.txt
	echo "#include \"vncmarshal.h\"" > $@ && \
	glib-genmarshal --body $< >> $@ || (rm -f $@ && exit 1)

vncmarshal.h: vncmarshal.txt
	glib-genmarshal --header $< > $@ || (rm -f $@ && exit 1)

vncdisplay.c:: vncmarshal.h enums.h

CLEANFILES = vncmarshal.c vncmarshal.h enums.h enums.c

if WITH_PYTHON
pyexec_LTLIBRARIES = gtkvnc.la

gtkvnc_la_LIBADD = libgtk-vnc-1.0.la @PYGTK_LIBS@
gtkvnc_la_CFLAGS = @GTK_CFLAGS@ @PYTHON_INCLUDES@ \
		   @PYGTK_CFLAGS@ @WARNING_PYTHON_CFLAGS@
gtkvnc_la_LDFLAGS = -module -avoid-version -fPIC
gtkvnc_la_SOURCES = vncmodule.c vncmodule.defs.c

CODEGENDIR = $(shell pkg-config --variable=codegendir pygtk-2.0)
DEFSDIR = $(shell pkg-config --variable=defsdir pygtk-2.0)

vnc.defs: vncdisplay.h enums.h
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

-include $(top_srcdir)/git.mk
