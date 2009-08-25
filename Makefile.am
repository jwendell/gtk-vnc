SUBDIRS = gnulib/lib src examples plugin gnulib/tests po
ACLOCAL_AMFLAGS = -I m4 -I gnulib/m4

pkgconfig_DATA = @PACKAGE@-1.0.pc
pkgconfigdir = $(libdir)/pkgconfig

EXTRA_DIST =				\
	$(PACKAGE)-1.0.pc.in		\
	$(PACKAGE).spec.in		\
	$(PACKAGE).spec			\
	mingw32-$(PACKAGE).spec.in	\
	intltool-extract.in		\
	intltool-merge.in		\
	intltool-update.in

DISTCLEANFILES =			\
	intltool-extract		\
	intltool-merge			\
	intltool-update

MAINTAINERCLEANFILES = 		\
	INSTALL			\
	compile			\
	aclocal.m4		\
	config.guess		\
	config.hin		\
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
	`find "$(srcdir)" -type f -name Makefile.in -print`

-include $(top_srcdir)/git.mk
