dnl Process this file with autoconf to produce a configure script.

AC_INIT([gtk-vnc], [0.3.8], [anthony@codemonkey.ws])
AC_CONFIG_SRCDIR([src/gvnc.c])

GTK_REQUIRED=2.10.0
AC_SUBST(GTK_REQUIRED)
GNUTLS_REQUIRED=1.4.0
AC_SUBST(GNUTLS_REQUIRED)

PYGTK_REQUIRED=2.0.0
VIEW_REQUIRED=0.6.0
GTHREAD_REQUIRED=2.0.0
PYTHON_REQUIRED=2.4

NSPR_REQUIRED=4.0.0
FIREFOX_PLUGIN_REQUIRED=2.0.0
MOZILLA_PLUGIN_REQUIRED=1.8


AC_CONFIG_HEADERS([config.h:config.hin])

AC_CANONICAL_TARGET

AM_INIT_AUTOMAKE(gtk-vnc, 0.3.8)


dnl gl_INIT uses m4_foreach_w, yet that is not defined in autoconf-2.59.
dnl In order to accommodate developers with such old tools, here's a
dnl replacement definition.
m4_ifndef([m4_foreach_w],
  [m4_define([m4_foreach_w],
    [m4_foreach([$1], m4_split(m4_normalize([$2]), [ ]), [$3])])])

gl_EARLY
gl_INIT

AC_PROG_CC_STDC
AM_PROG_CC_C_O
AC_LIBTOOL_WIN32_DLL

AC_PROG_LIBTOOL

AC_CHECK_HEADERS([pwd.h winsock2.h])

AC_ARG_WITH(python,
[  --with-python           build python bindings],
[case "${withval}" in
   yes|no) ;;
   *)      AC_MSG_ERROR([bad value ${withval} for python option]) ;;
 esac],[withval=yes])

WITH_PYTHON=$withval

AC_ARG_ENABLE(fatal-warnings,
[  --enable-fatal-warnings make all compiler warnings fatal (not recommended)],
[case "${enableval}" in
   yes|no) ;;
   *)      AC_MSG_ERROR([bad value ${enableval} for fatal-warnings option]) ;;
 esac],
              [enableval=no])
if test "${enableval}" = yes; then
  gl_COMPILER_FLAGS(-Werror)
  AC_SUBST([WERROR_CFLAGS], [$COMPILER_FLAGS])
  COMPILER_FLAGS=
fi

AC_ARG_ENABLE(warnings,
[  --enable-warnings       turn on lots of compiler warnings (recommended)],
[case "${enableval}" in
   yes|no) ;;
   *)      AC_MSG_ERROR([bad value ${enableval} for warnings option]) ;;
 esac],
              [enableval=no])

if test "${enableval}" = yes; then
  gl_COMPILER_FLAGS(-W)
  gl_COMPILER_FLAGS(-Wall)
  gl_COMPILER_FLAGS(-Wcast-align)
  gl_COMPILER_FLAGS(-Wformat)
  gl_COMPILER_FLAGS(-Wwrite-strings)
  AC_SUBST([WARNING_CXXFLAGS], [$COMPILER_FLAGS])
  # The following warnings are not suitable for C++.
  gl_COMPILER_FLAGS(-Wbad-function-cast)
  gl_COMPILER_FLAGS(-Wmissing-declarations)
  gl_COMPILER_FLAGS(-Wmissing-prototypes)
  gl_COMPILER_FLAGS(-Wshadow)
  # gl_COMPILER_FLAGS(-Wstrict-prototypes)
  AC_SUBST([WARNING_CFLAGS], [$COMPILER_FLAGS])
  AC_DEFINE([lint], 1, [Define to 1 if the compiler is checking for lint.])
  COMPILER_FLAGS=
fi

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
AC_SUBST(GNUTLS_CFLAGS)
AC_SUBST(GNUTLS_LIBS)

dnl Cyrus SASL
AC_ARG_WITH([sasl],
  [  --with-sasl         use cyrus SASL for authentication],
  [],
  [with_sasl=check])

SASL_CFLAGS=
SASL_LIBS=
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
    AC_CHECK_LIB([sasl2], [sasl_client_init],[with_sasl=yes],[
      if test "x$with_sasl" = "xcheck" ; then
          with_sasl=no
      else
          fail=1
      fi])
  fi
  test $fail = 1 &&
    AC_MSG_ERROR([You must install the Cyrus SASL development package in order to compile GTK-VNC])
  CFLAGS="$old_cflags"
  LIBS="$old_libs"
  SASL_LIBS="$SASL_LIBS -lsasl2"
  if test "x$with_sasl" = "xyes" ; then
    AC_DEFINE_UNQUOTED([HAVE_SASL], 1,
      [whether Cyrus SASL is available for authentication])
  fi
fi
AM_CONDITIONAL([HAVE_SASL], [test "x$with_sasl" = "xyes"])
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
    dnl Check for Netscape Portable Runtime development package.
    PKG_CHECK_MODULES(NSPR, nspr >= $NSPR_REQUIRED)
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
"
