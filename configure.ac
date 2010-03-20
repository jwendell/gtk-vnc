dnl Process this file with autoconf to produce a configure script.

AC_INIT([gtk-vnc], [0.3.10])
AC_CONFIG_SRCDIR([src/gvnc.c])
dnl Causes problems with RHEL-5 vintage intltool
dnl AC_CONFIG_AUX_DIR([build-aux])
AM_CONFIG_HEADER([config.h])
dnl Make automake keep quiet about wildcards & other GNUmake-isms
AM_INIT_AUTOMAKE([-Wno-portability])

# Use the silent-rules feature when possible.
m4_ifndef([AM_SILENT_RULES], [m4_define([AM_SILENT_RULES],[])])
AM_SILENT_RULES([yes])

AC_CANONICAL_HOST

dnl *******************************************************************************
dnl Declare required library versions
dnl *******************************************************************************

GTK_REQUIRED=2.14.0
AC_SUBST(GTK_REQUIRED)
GNUTLS_REQUIRED=1.4.0
AC_SUBST(GNUTLS_REQUIRED)

PYGTK_REQUIRED=2.0.0
VIEW_REQUIRED=0.6.0
GTHREAD_REQUIRED=2.0.0
PYTHON_REQUIRED=2.4

FIREFOX_PLUGIN_REQUIRED=2.0.0
MOZILLA_PLUGIN_REQUIRED=1.8

dnl *******************************************************************************
dnl Setup GNULIB - must be before anything else in this file
dnl *******************************************************************************

dnl gl_INIT uses m4_foreach_w, yet that is not defined in autoconf-2.59.
dnl In order to accommodate developers with such old tools, here's a
dnl replacement definition.
m4_ifndef([m4_foreach_w],
  [m4_define([m4_foreach_w],
    [m4_foreach([$1], m4_split(m4_normalize([$2]), [ ]), [$3])])])

gl_EARLY
gl_INIT

dnl *******************************************************************************
dnl Setup core compilers / build helpers
dnl *******************************************************************************

AC_PROG_CC_STDC
AM_PROG_CC_C_O

AC_LIBTOOL_WIN32_DLL
AC_PROG_LIBTOOL

dnl *******************************************************************************
dnl Internationalization
dnl *******************************************************************************
GETTEXT_PACKAGE=gtk-vnc
AC_SUBST(GETTEXT_PACKAGE)
AC_DEFINE_UNQUOTED(GETTEXT_PACKAGE,"$GETTEXT_PACKAGE", [GETTEXT package name])

IT_PROG_INTLTOOL([0.35.0])
AM_GLIB_GNU_GETTEXT

dnl *******************************************************************************


AC_CHECK_HEADERS([pwd.h winsock2.h])

AC_ARG_WITH(python,
[  --with-python           build python bindings],
[case "${withval}" in
   yes|no) ;;
   *)      AC_MSG_ERROR([bad value ${withval} for python option]) ;;
 esac],[withval=yes])

WITH_PYTHON=$withval

GTK_VNC_COMPILE_WARNINGS

AC_ARG_WITH(examples,
[  --with-examples         install example programs],
[case "${withval}" in
   yes|no) ;;
   *)      AC_MSG_ERROR([bad value ${withval} for examples option]) ;;
 esac],[withval=no])
WITH_EXAMPLES=$withval


dnl check ld support --version-script
ld --help>/tmp/$$ 2>&1
if `cat /tmp/$$|grep version-script>/dev/null 2>&1`; then
LD_VERSION_SCRIPT_SUPPORT='--version-script=$(srcdir)/libgtk-vnc_sym.version'
else
LD_VERSION_SCRIPT_SUPPORT="export-symbols-regex 'vnc_'"
fi
rm /tmp/$$
AC_SUBST(LD_VERSION_SCRIPT_SUPPORT)

PKG_CHECK_MODULES(GTK, gtk+-2.0 >= $GTK_REQUIRED)
AC_SUBST(GTK_CFLAGS)
AC_SUBST(GTK_LIBS)

PKG_CHECK_MODULES(X11, x11,,AC_MSG_NOTICE([Not building against X11]))
AC_SUBST(X11_CFLAGS)
AC_SUBST(X11_LIBS)

save_CFLAGS="$CFLAGS"
save_LIBS="$LIBS"
CFLAGS="$CFLAGS $GTK_CFLAGS"
LIBS="$LIBS $GTK_LIBS"
AC_CHECK_FUNC([gdk_cairo_create], [WITH_GTK_CAIRO=1], [WITH_GTK_CAIRO=0])
CFLAGS="$save_CFLAGS"
LIBS="$save_LIBS"

AC_DEFINE_UNQUOTED(WITH_GTK_CAIRO, [$WITH_GTK_CAIRO], [Whether to use cairo for GTK rendering])


AC_ARG_WITH(libview,
[  --with-libview          enable libview support in gvncviewer],
[case "${withval}" in
   yes|no) ;;
   *)	   AC_MSG_ERROR([bad value ${withval} for libview option]) ;;
 esac],[withval=yes])

WITH_LIBVIEW=0
if test "${withval}" = "yes"; then
  PKG_CHECK_MODULES(VIEW, libview >= $VIEW_REQUIRED,
		   [WITH_LIBVIEW=1], [WITH_LIBVIEW=0])
fi
AC_DEFINE_UNQUOTED(WITH_LIBVIEW,[$WITH_LIBVIEW], [Whether to use libview])
AC_SUBST(VIEW_CFLAGS)
AC_SUBST(VIEW_LIBS)

PKG_CHECK_MODULES(GNUTLS, gnutls >= $GNUTLS_REQUIRED)
dnl Not all versions of gnutls include -lgcrypt, and we need it
dnl explicitly for building MS Logon extension which calls
dnl gcry_XXX APIs
GNUTLS_LIBS="$GNUTLS_LIBS -lgcrypt"
AC_SUBST(GNUTLS_CFLAGS)
AC_SUBST(GNUTLS_LIBS)

dnl Cyrus SASL
AC_ARG_WITH([sasl],
  [AS_HELP_STRING([--with-sasl],
    [use cyrus SASL for authentication @<:@default=check@:>@])],
  [],
  [with_sasl=check])

SASL_CFLAGS=
SASL_LIBS=
enable_sasl=no
if test "x$with_sasl" != "xno"; then
  if test "x$with_sasl" != "xyes" -a "x$with_sasl" != "xcheck"; then
    SASL_CFLAGS="-I$with_sasl"
    SASL_LIBS="-L$with_sasl"
  fi
  fail=0
  old_cflags="$CFLAGS"
  old_libs="$LIBS"
  CFLAGS="$CFLAGS $SASL_CFLAGS"
  LIBS="$LIBS $SASL_LIBS"
  AC_CHECK_HEADER([sasl/sasl.h],[],[
    if test "x$with_sasl" != "xcheck" ; then
        with_sasl=no
    else
        fail=1
    fi])
  if test "x$with_sasl" != "xno" ; then
    AC_CHECK_LIB([sasl2], [sasl_client_init],[with_sasl2=yes],[with_sasl2=no])
  fi
  if test "x$with_sasl2" = "xno" -a "x$with_sasl" != "xno" ; then
    AC_CHECK_LIB([sasl], [sasl_client_init],[with_sasl=yes],[with_sasl=no])
  fi
  if test "x$with_sasl2" = "xyes"; then
    SASL_LIBS="$SASL_LIBS -lsasl2"
  elif test "x$with_sasl" = "xyes"; then
    SASL_LIBS="$SASL_LIBS -lsasl"
  else
    AC_MSG_ERROR([You must install the Cyrus SASL development package in order to compile GTK-VNC])
  fi
  CFLAGS="$old_cflags"
  LIBS="$old_libs"
  if test "x$with_sasl2" = "xyes" -o "x$with_sasl" = "xyes" ; then
    AC_DEFINE_UNQUOTED([HAVE_SASL], 1,
      [whether Cyrus SASL is available for authentication])
    enable_sasl=yes
  fi
fi
AM_CONDITIONAL([HAVE_SASL], [test "x$with_sasl2" = "xyes" -o "x$with_sasl" = "xyes"])
AC_SUBST([SASL_CFLAGS])
AC_SUBST([SASL_LIBS])


GTHREAD_CFLAGS=
GTHREAD_LIBS=

AC_CHECK_LIB(z, inflate, [], [AC_MSG_ERROR([zlib not found])])

WITH_UCONTEXT=1

AC_ARG_WITH(coroutine,
[  --with-coroutine=ucontext/gthread  use ucontext or GThread for coroutines],
[],[with_coroutine=ucontext])

case $with_coroutine in
  ucontext)
    ;;
  gthread)
    ;;
  *)
    AC_ERROR([Unsupported coroutine type])
esac

if test "$with_coroutine" = "ucontext"; then
  AC_CHECK_FUNC(makecontext, [],[with_coroutine=gthread])
  AC_CHECK_FUNC(swapcontext, [],[with_coroutine=gthread])
  AC_CHECK_FUNC(getcontext, [],[with_coroutine=gthread])
fi

if test "$with_coroutine" = "gthread"; then
  PKG_CHECK_MODULES(GTHREAD, gthread-2.0 > $GTHREAD_REQUIRED)
  WITH_UCONTEXT=0
fi
AC_SUBST(GTHREAD_CFLAGS)
AC_SUBST(GTHREAD_LIBS)
AC_DEFINE_UNQUOTED(WITH_UCONTEXT,[$WITH_UCONTEXT], [Whether to use ucontext coroutine impl])
AM_CONDITIONAL(WITH_UCONTEXT, [test "$WITH_UCONTEXT" != "0"])

if test "$WITH_PYTHON" = "yes"; then
  PKG_CHECK_MODULES(PYGTK, pygtk-2.0 >= $PYGTK_REQUIRED)
  AC_SUBST(PYGTK_CFLAGS)
  AC_SUBST(PYGTK_LIBS)

  AM_PATH_PYTHON

  AC_MSG_CHECKING([whether $PYTHON version >= $PYTHON_REQUIRED])
  HAVE_PYTHON_REQUIRED=no
  AM_PYTHON_CHECK_VERSION([$PYTHON], [$PYTHON_REQUIRED],
      [HAVE_PYTHON_REQUIRED="yes"],
      [HAVE_PYTHON_REQUIRED="no"])

  AC_MSG_RESULT($HAVE_PYTHON_REQUIRED)

  if test "x$HAVE_PYTHON_REQUIRED" != "xyes"
  then
    AC_MSG_ERROR("No suitable python found")
  fi

  AM_CHECK_PYTHON_HEADERS(have_python_headers=yes,have_python_headers=no)

  if test "x$have_python_headers" != "xyes"
  then
    AC_MSG_ERROR("No python development headers found")
  fi
fi

dnl --enable-plugin to enable the browser plugin.
AC_ARG_ENABLE(plugin,
	[  --enable-plugin=[no/yes] enable browser plugin [default=no]],,
	enable_plugin=no)
if test "x$enable_plugin" = "xyes"; then
    dnl Check for Firefox plugin package (also includes NSPR cflags/libs).
    PKG_CHECK_MODULES(FIREFOX_PLUGIN,
                     firefox-plugin >= $FIREFOX_PLUGIN_REQUIRED, ,[
        PKG_CHECK_MODULES(MOZILLA_PLUGIN,
                          mozilla-plugin >= $MOZILLA_PLUGIN_REQUIRED)
        FIREFOX_PLUGIN_CFLAGS="$MOZILLA_PLUGIN_CFLAGS"
        FIREFOX_PLUGIN_LIBS="$MOZILLA_PLUGIN_LIBS"
    ])
    AC_SUBST(FIREFOX_PLUGIN_CFLAGS)
    AC_SUBST(FIREFOX_PLUGIN_LIBS)
fi

dnl define a MAINT-like variable REBUILD which is set
dnl if Perl, so autogenerated sources can be rebuilt
AC_PATH_PROGS(PERL, perl5 perl)
AC_ARG_ENABLE(rebuilds, [  --disable-rebuilds      disable all source autogeneration rules],,enable_rebuilds=yes)
REBUILD=\#
if test "x$enable_rebuilds" = "xyes" && \
     test -n "$PERL" && \
     $PERL -e 'exit !($] >= 5.002)' > /dev/null 2>&1 ; then
  REBUILD=
fi
AC_SUBST(REBUILD)


AM_CONDITIONAL(WITH_PYTHON, [test "$WITH_PYTHON" = "yes"])

AM_CONDITIONAL(WITH_EXAMPLES, [test "$WITH_EXAMPLES" = "yes"])

AM_CONDITIONAL(ENABLE_PLUGIN, [test "$enable_plugin" = "yes"])

AC_CONFIG_FILES(
  Makefile
  gnulib/lib/Makefile
  gnulib/tests/Makefile
  src/Makefile
  examples/Makefile
  plugin/Makefile
  po/Makefile.in
  gtk-vnc-1.0.pc
  gtk-vnc.spec
  mingw32-gtk-vnc.spec
  )

AC_OUTPUT

if [ test "$WITH_GTK_CAIRO" = 1 ]; then
  with_scaling=yes
else
  with_scaling=no
fi

echo "
Configure summary:

	Compiler ...................:  ${CC}
	Compiler Flags .............:  ${CFLAGS}
	Prefix......................:  ${prefix}

	Python binding .............:  ${WITH_PYTHON}
	Install example programs ...:  ${WITH_EXAMPLES}
	Browser plugin .............:  ${enable_plugin}
	Scaling support.............:  ${with_scaling}
	SASL support................:  ${enable_sasl}
"
