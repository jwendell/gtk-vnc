SUBDIRS = gnulib/lib src tools examples plugin gnulib/tests po
ACLOCAL_AMFLAGS = -I gnulib/m4 ${ACLOCAL_FLAGS}

pkgconfig_DATA = @PACKAGE@-1.0.pc gvnc-1.0.pc
pkgconfigdir = $(libdir)/pkgconfig

EXTRA_DIST =				\
	gvnc-1.0.pc.in			\
	$(PACKAGE)-1.0.pc.in		\
	$(PACKAGE).spec.in		\
	$(PACKAGE).spec			\
	mingw32-$(PACKAGE).spec.in	\
	intltool-extract.in		\
	intltool-merge.in		\
	intltool-update.in		\
	ChangeLog-old

DISTCLEANFILES =			\
	intltool-extract		\
	intltool-merge			\
	intltool-update

MAINTAINERCLEANFILES = 		\
	INSTALL			\
	compile			\
	aclocal.m4		\
	config.guess		\
	config.h.in		\
	config.sub		\
	depcomp			\
	gtk-doc.make		\
	install-sh		\
	ltmain.sh		\
	missing			\
	mkinstalldirs		\
	omf.make		\
	py-compile		\
	xmldocs.make		\
	gnulib/m4/gnome-doc-utils.m4	\
	gnulib/m4/gtk-doc.m4		\
	gnulib/m4/intltool.m4		\
	gnulib/m4/libtool.m4		\
	gnulib/m4/ltoptions.m4		\
	gnulib/m4/ltsugar.m4		\
	gnulib/m4/ltversion.m4		\
	gnulib/m4/lt~obsolete.m4	\
	m4/libtool.m4			\
	m4/lt~obsolete.m4		\
	m4/ltoptions.m4			\
	m4/ltsugar.m4			\
	m4/ltversion.m4			\
	build-aux/*			\
	`find "$(srcdir)" -type f -name Makefile.in -print`

GITIGNOREFILES = plugin/gtk-vnc-plugin.la plugin/gtk-vnc-plugin.so

gen_start_date = 2008-12-07
dist-hook:
	$(top_srcdir)/build-aux/gitlog-to-changelog           \
            --since=$(gen_start_date) > $(distdir)/cl-t;      \
        rm -f $(distdir)/ChangeLog;                           \
        mv $(distdir)/cl-t $(distdir)/ChangeLog;

-include $(top_srcdir)/git.mk
