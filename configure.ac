dnl Process this file with autoconf to produce a configure script.

AC_INIT([gtk-vnc], [0.0.1], [anthony@codemonkey.ws])
AC_CONFIG_SRCDIR([src/gvnc.c])

GTK_REQUIRED=2.0.0
AC_SUBST(GTK_REQUIRED)
GNUTLS_REQUIRED=1.4.0
AC_SUBST(GNUTLS_REQUIRED)

PYGTK_REQUIRED=2.0.0
PYTHON_REQUIRED=2.4

AC_CONFIG_HEADERS([config.h:config.hin])


AC_CANONICAL_TARGET

AM_INIT_AUTOMAKE(gtk-vnc, 0.0.1)

AC_PROG_CC_STDC
AC_PROG_LIBTOOL

AC_ARG_WITH(python,
[  --with-python       build python bindings],
[case "${withval}" in
   yes|no) ;;
   *)      AC_MSG_ERROR([bad value ${withval} for python option]) ;;
 esac],[withval=yes])

WITH_PYTHON=$withval

AC_ARG_ENABLE(fatal-warnings,
[  --enable-warnings   make all compiler warnings fatal (not recommended)],
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
[  --enable-warnings   turn on lots of compiler warnings (recommended)],
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

PKG_CHECK_MODULES(GTK, gtk+-2.0 >= $GTK_REQUIRED)
AC_SUBST(GTK_CFLAGS)
AC_SUBST(GTK_LIBS)

PKG_CHECK_MODULES(GNUTLS, gnutls >= $GNUTLS_REQUIRED)
AC_SUBST(GNUTLS_CFLAGS)
AC_SUBST(GNUTLS_LIBS)

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

AM_CONDITIONAL(WITH_PYTHON, [test "$WITH_PYTHON" = "yes"])

AC_CONFIG_FILES(
  Makefile
  src/Makefile
  gtk-vnc-1.0.pc
  gtk-vnc.spec
  )

AC_OUTPUT
