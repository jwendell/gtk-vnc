
EXTRA_DIST = libgvnc_sym.version libgtk-vnc_sym.version vncmarshal.txt

lib_LTLIBRARIES = libgvnc-1.0.la

BUILT_SOURCES =
if USE_VERSION_DEFS
GVNC_VERSION_FILE = $(builddir)/libgvnc_sym.def
GTK_VNC_VERSION_FILE = $(builddir)/libgtk-vnc_sym.def
BUILT_SOURCES += $(GVNC_VERSION_FILE) $(GTK_VNC_VERSION_FILE)
else
GVNC_VERSION_FILE = $(srcdir)/libgvnc_sym.version
GTK_VNC_VERSION_FILE = $(srcdir)/libgtk-vnc_sym.version
endif

%.def: %.version
	$(AM_V_GEN)rm -f -- $@-tmp $@ ; \
	printf 'EXPORTS\n' > $@-tmp && \
	sed -e '/^$$/d; /#/d; /:/d; /\}/d; /\*/d; /LIBVIRT_/d; s/[ \t]*\(.*\)\;/    \1/g' $^ >> $@-tmp && \
	chmod a-w $@-tmp && \
	mv $@-tmp $@


libgvnc_1_0_la_LIBADD = \
			@GOBJECT_LIBS@ \
			@GTHREAD_LIBS@ \
			@GDK_PIXBUF_LIBS@ \
			@GNUTLS_LIBS@ \
			@SASL_LIBS@ \
			../gnulib/lib/libgnu.la
libgvnc_1_0_la_CFLAGS = \
			@GOBJECT_CFLAGS@ \
			@GTHREAD_CFLAGS@ \
			@GDK_PIXBUF_CFLAGS@ \
			@GNUTLS_CFLAGS@ \
			@SASL_CFLAGS@ \
			@WARNING_CFLAGS@ \
			-DSYSCONFDIR=\""$(sysconfdir)"\" \
			-DPACKAGE_LOCALE_DIR=\""$(datadir)/locale"\" \
			-DG_LOG_DOMAIN=\"gvnc\" \
			-I$(top_srcdir)/gnulib/lib \
			-I../gnulib/lib
libgvnc_1_0_la_LDFLAGS = \
			$(VERSION_SCRIPT_FLAGS)$(GVNC_VERSION_FILE) \
			-version-info 0:1:0 $(NO_UNDEFINED_FLAGS)

gvnc_includedir = $(includedir)/gvnc-1.0/
gvnc_include_HEADERS = \
			vncpixelformat.h \
			vncframebuffer.h \
			vncbaseframebuffer.h \
			vnccursor.h \
			vnccolormap.h \
			vncconnection.h \
			vncconnectionenums.h \
			vncutil.h

libgvnc_1_0_la_SOURCES = \
			coroutine.h \
			d3des.h d3des.c \
			dh.h dh.c \
			vncpixelformat.h vncpixelformat.c \
			vncframebuffer.h vncframebuffer.c \
			vncbaseframebufferblt.h \
			vncbaseframebuffer.h vncbaseframebuffer.c \
			vnccursor.h vnccursor.c \
			vnccolormap.h vnccolormap.c \
			vncconnection.h vncconnection.c \
			vncconnectionblt.h \
			vncconnectionenums.h vncconnectionenums.c \
			vncmarshal.h vncmarshal.c \
			vncutil.h vncutil.c

if WITH_UCONTEXT
libgvnc_1_0_la_SOURCES += continuation.h continuation.c coroutine_ucontext.c
EXTRA_DIST += coroutine_gthread.c
else
libgvnc_1_0_la_SOURCES += coroutine_gthread.c
EXTRA_DIST += continuation.h continuation.c coroutine_ucontext.c
endif

gtk_vnc_LIBADD = \
			@GTK_LIBS@ \
			@X11_LIBS@ \
			libgvnc-1.0.la \
			../gnulib/lib/libgnu.la
gtk_vnc_CFLAGS = \
			@GTK_CFLAGS@ \
			@X11_CFLAGS@ \
			@WARNING_CFLAGS@ \
			-DSYSCONFDIR=\""$(sysconfdir)"\" \
			-DPACKAGE_LOCALE_DIR=\""$(datadir)/locale"\" \
			-DG_LOG_DOMAIN=\"gtk-vnc\" \
			-I$(top_srcdir)/gnulib/lib \
			-I../gnulib/lib

gtk_vnc_includedir = $(includedir)/gtk-vnc-$(GTK_VNC_API_VERSION)/
gtk_vnc_include_HEADERS = \
			vncdisplay.h \
			vncdisplayenums.h \
			vncgrabsequence.h \
			vncimageframebuffer.h

gtk_vnc_SOURCES = \
			vncimageframebuffer.h vncimageframebuffer.c \
			vncdisplay.h vncdisplay.c \
			vncdisplayenums.h vncdisplayenums.c \
			vncdisplaykeymap.h vncdisplaykeymap.c \
			vncgrabsequence.h vncgrabsequence.c \
			vncmarshal.h vncmarshal.c

gtk_vnc_LDFLAGS = \
			$(VERSION_SCRIPT_FLAGS)$(GTK_VNC_VERSION_FILE) \
			$(NO_UNDEFINED_FLAGS)

if HAVE_GTK_2
lib_LTLIBRARIES += libgtk-vnc-1.0.la
libgtk_vnc_1_0_la_LIBADD = $(gtk_vnc_LIBADD)
libgtk_vnc_1_0_la_CFLAGS = $(gtk_vnc_CFLAGS)
libgtk_vnc_1_0_la_LDFLAGS = $(gtk_vnc_LDFLAGS) -version-info 0:1:0
libgtk_vnc_1_0_la_SOURCES = $(gtk_vnc_SOURCES)
else
lib_LTLIBRARIES += libgtk-vnc-2.0.la
libgtk_vnc_2_0_la_LIBADD = $(gtk_vnc_LIBADD)
libgtk_vnc_2_0_la_CFLAGS = $(gtk_vnc_CFLAGS)
libgtk_vnc_2_0_la_LDFLAGS = $(gtk_vnc_LDFLAGS) -version-info 0:2:0
libgtk_vnc_2_0_la_SOURCES = $(gtk_vnc_SOURCES)
endif


vncdisplayenums.c: vncdisplay.h
	$(AM_V_GEN)glib-mkenums    --fhead "#include <glib-object.h>\n" \
			--fhead "#include \"vncdisplayenums.h\"\n\n" \
			--fprod "\n/* enumerations from \"vncdisplay.h\" */" \
			--fprod "\n#include \"vncdisplay.h\"\n" \
			--vhead "static const G@Type@Value _@enum_name@_values[] = {" \
			--vprod "  { @VALUENAME@, \"@VALUENAME@\", \"@valuenick@\" }," \
			--vtail "  { 0, NULL, NULL }\n};\n\n" \
			--vtail "GType\n@enum_name@_get_type (void)\n{\n" \
			--vtail "  static GType type = 0;\n\n" \
			--vtail "  if (!type)\n" \
			--vtail "    type = g_@type@_register_static (\"@EnumName@\", _@enum_name@_values);\n\n" \
			--vtail "  return type;\n}\n\n" \
		$< > $@

vncdisplayenums.h: vncdisplay.h
	$(AM_V_GEN)glib-mkenums    --fhead "#ifndef VNC_DISPLAY_ENUMS_H\n" \
			--fhead "#define VNC_DISPLAY_ENUMS_H\n\n" \
			--fhead "G_BEGIN_DECLS\n\n" \
			--ftail "G_END_DECLS\n\n" \
			--ftail "#endif /* VNC_DISPLAY_ENUMS_H */\n" \
			--fprod "\n/* --- vncdisplay.h --- */" \
			--eprod "#define VNC_TYPE_@ENUMSHORT@ @enum_name@_get_type()\n" \
			--eprod "GType @enum_name@_get_type (void);\n" \
		$< >  $@

vncconnectionenums.c: vncconnection.h
	$(AM_V_GEN)glib-mkenums    --fhead "#include <glib-object.h>\n" \
			--fhead "#include \"vncconnectionenums.h\"\n\n" \
			--fprod "\n/* enumerations from \"vncconnection.h\" */" \
			--fprod "\n#include \"vncconnection.h\"\n" \
			--vhead "static const G@Type@Value _@enum_name@_values[] = {" \
			--vprod "  { @VALUENAME@, \"@VALUENAME@\", \"@valuenick@\" }," \
			--vtail "  { 0, NULL, NULL }\n};\n\n" \
			--vtail "GType\n@enum_name@_get_type (void)\n{\n" \
			--vtail "  static GType type = 0;\n\n" \
			--vtail "  if (!type)\n" \
			--vtail "    type = g_@type@_register_static (\"@EnumName@\", _@enum_name@_values);\n\n" \
			--vtail "  return type;\n}\n\n" \
		$< > $@

vncconnectionenums.h: vncconnection.h
	$(AM_V_GEN)glib-mkenums    --fhead "#ifndef VNC_CONNECTION_ENUMS_H\n" \
			--fhead "#define VNC_CONNECTION_ENUMS_H\n\n" \
			--fhead "G_BEGIN_DECLS\n\n" \
			--ftail "G_END_DECLS\n\n" \
			--ftail "#endif /* VNC_CONNECTION_ENUMS_H */\n" \
			--fprod "\n/* --- vncconnection.h --- */" \
			--eprod "#define VNC_TYPE_@ENUMSHORT@ @enum_name@_get_type()\n" \
			--eprod "GType @enum_name@_get_type (void);\n" \
		$< >  $@

vncmarshal.c: vncmarshal.txt
	$(AM_V_GEN)echo "#include \"vncmarshal.h\"" > $@ && \
		glib-genmarshal --body $< >> $@ || (rm -f $@ && exit 1)

vncmarshal.h: vncmarshal.txt
	$(AM_V_GEN)glib-genmarshal --header $< > $@ || (rm -f $@ && exit 1)

vncdisplay.c:: vncmarshal.h vncdisplayenums.h

vncconnection.c:: vncmarshal.h vncconnectionenums.h

CLEANFILES = vncmarshal.c vncmarshal.h \
	vncdisplayenums.h vncdisplayenums.c \
	vncconnectionenums.h vncconnectionenums.c

if WITH_PYTHON
pyexec_LTLIBRARIES = gtkvnc.la

gtkvnc_la_LIBADD = libgtk-vnc-$(GTK_VNC_API_VERSION).la @PYGTK_LIBS@
gtkvnc_la_CFLAGS = @GTK_CFLAGS@ @PYTHON_INCLUDES@ \
		   @PYGTK_CFLAGS@ @WARNING_PYTHON_CFLAGS@
gtkvnc_la_LDFLAGS = -module -avoid-version -fPIC
gtkvnc_la_SOURCES = vncmodule.c vncmodule.defs.c

CODEGENDIR = $(shell pkg-config --variable=codegendir pygtk-2.0)
DEFSDIR = $(shell pkg-config --variable=defsdir pygtk-2.0)

vnc.defs: vncdisplay.h vncdisplayenums.h
	$(AM_V_GEN)$(PYTHON) $(CODEGENDIR)/h2def.py $< > $@

vncmodule.defs.c: vnc.override vnc.defs
	$(AM_V_GEN)pygtk-codegen-2.0 --prefix gtkvnc \
			  --register $(DEFSDIR)/gdk-types.defs \
			  --register $(DEFSDIR)/gtk-types.defs \
			  --override $(srcdir)/vnc.override vnc.defs > $@

CLEANFILES += vnc.defs vncmodule.defs.c
EXTRA_DIST += vnc.override

else
EXTRA_DIST += vnc.override vncmodule.c
endif

if WITH_GOBJECT_INTROSPECTION

GVNC_INTROSPECTION_SRCS = \
			$(srcdir)/vncpixelformat.h \
			$(srcdir)/vncframebuffer.h $(srcdir)/vncframebuffer.c \
			$(srcdir)/vncbaseframebuffer.h $(srcdir)/vncbaseframebuffer.c \
			$(srcdir)/vnccolormap.h $(srcdir)/vnccolormap.c \
			$(srcdir)/vnccursor.h $(srcdir)/vnccursor.c \
			$(srcdir)/vncconnection.h $(srcdir)/vncconnection.c \
			$(builddir)/vncconnectionenums.h $(builddir)/vncconnectionenums.c \
			$(srcdir)/vncutil.h $(srcdir)/vncutil.c

GTK_VNC_INTROSPECTION_SRCS = \
			$(srcdir)/vncimageframebuffer.h $(srcdir)/vncimageframebuffer.c \
			$(srcdir)/vncdisplay.h $(srcdir)/vncdisplay.c \
			$(srcdir)/vncgrabsequence.h $(srcdir)/vncgrabsequence.c \
			$(builddir)/vncdisplayenums.h $(builddir)/vncdisplayenums.c

GVnc-1.0.gir: libgvnc-1.0.la $(G_IR_SCANNER) Makefile.am
	$(AM_V_GEN)$(G_IR_SCANNER) -v \
                --namespace GVnc \
                --nsversion 1.0 \
		--include GObject-2.0 \
		--strip-prefix=Vnc \
                --library=$(builddir)/libgvnc-1.0.la \
                --add-include-path=$(top_srcdir) \
                --add-include-path=$(srcdir) \
                --add-include-path=$(builddir) \
                --output $@ \
                -I$(top_srcdir) \
                -I$(srcdir) \
		--verbose \
                --pkg=glib-2.0 \
                --pkg=gthread-2.0 \
                --pkg=gdk-pixbuf-2.0 \
                --pkg=gobject-2.0 \
                $(GVNC_INTROSPECTION_SRCS)

GtkVnc-$(GTK_VNC_API_VERSION).gir: GVnc-1.0.gir libgtk-vnc-$(GTK_VNC_API_VERSION).la $(G_IR_SCANNER) Makefile.am
	$(AM_V_GEN)$(G_IR_SCANNER) -v \
                --namespace GtkVnc \
                --nsversion $(GTK_VNC_API_VERSION) \
		--include GObject-2.0 \
		--include Gtk-$(GTK_API_VERSION) \
		--include GVnc-1.0 \
		--strip-prefix=Vnc \
                --library=$(builddir)/libgtk-vnc-$(GTK_VNC_API_VERSION).la \
                --add-include-path=$(top_srcdir) \
                --add-include-path=$(srcdir) \
                --add-include-path=$(builddir) \
                --output $@ \
                -I$(top_srcdir) \
                -I$(srcdir) \
		--verbose \
                --pkg=glib-2.0 \
                --pkg=gthread-2.0 \
                --pkg=gdk-pixbuf-2.0 \
                --pkg=gobject-2.0 \
                --pkg=gtk+-$(GTK_API_VERSION) \
                $(GTK_VNC_INTROSPECTION_SRCS)

girdir = $(datadir)/gir-1.0
gir_DATA = GVnc-1.0.gir GtkVnc-$(GTK_VNC_API_VERSION).gir

typelibsdir = $(libdir)/girepository-1.0
typelibs_DATA = GVnc-1.0.typelib GtkVnc-$(GTK_VNC_API_VERSION).typelib

%.typelib: %.gir
	$(AM_V_GEN)$(G_IR_COMPILER) \
		--includedir=$(srcdir) \
		--includedir=$(builddir) \
		--includedir=$(girdir) \
		-o $@ $<

CLEANFILES += $(gir_DATA) $(typelibs_DATA)

endif # WITH_GOBJECT_INTROSPECTION

-include $(top_srcdir)/git.mk
