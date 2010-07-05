
POD2MAN = pod2man -c "VNC Tools" -r "$(PACKAGE)-$(VERSION)"

bin_PROGRAMS = gvnccapture

man1_MANS = gvnccapture.1

CLEANFILES = $(man1_MANS)

gvnccapture.1: gvnccapture.c
	$(AM_V_GEN)$(POD2MAN) --name 'gvnccapture' $< $@

gvnccapture_SOURCES = gvnccapture.c
gvnccapture_LDADD = \
		../src/libgvnc-1.0.la \
		$(GOBJECT_LIBS) \
		$(GDK_PIXBUF_LIBS)
gvnccapture_CFLAGS = \
		$(GOBJECT_CFLAGS) \
		$(GDK_PIXBUF_CFLAGS) \
		$(WARNING_CFLAGS) \
		-I$(top_srcdir)/src/

-include $(top_srcdir)/git.mk
