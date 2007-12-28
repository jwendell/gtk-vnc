dnl Process this file with autoconf to produce a configure script.

AC_INIT([gtk-vnc], [0.3.2], [anthony@codemonkey.ws])
AC_CONFIG_SRCDIR([src/gvnc.c])

GTK_REQUIRED=2.0.0
AC_SUBST(GTK_REQUIRED)
GNUTLS_REQUIRED=1.4.0
AC_SUBST(GNUTLS_REQUIRED)

PYGTK_REQUIRED=2.0.0
GTHREAD_REQUIRED=2.0.0
PYTHON_REQUIRED=2.4

AC_CONFIG_HEADERS([config.h:config.hin])


AC_CANONICAL_TARGET

AM_INIT_AUTOMAKE(gtk-vnc, 0.3.2)

AC_PROG_CC_STDC
AM_PROG_CC_C_O
AC_PROG_LIBTOOL

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

PKG_CHECK_MODULES(GTK, gtk+-2.0 >= $GTK_REQUIRED)
AC_SUBST(GTK_CFLAGS)
AC_SUBST(GTK_LIBS)

PKG_CHECK_MODULES(GNUTLS, gnutls >= $GNUTLS_REQUIRED)
AC_SUBST(GNUTLS_CFLAGS)
AC_SUBST(GNUTLS_LIBS)

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

dnl --enable-debug=(yes|no)
AC_ARG_ENABLE(debug, [  --enable-debug=[no/yes] turn on debugging [default=no]],,enable_debug=no)
if test "$enable_debug" = "yes"; then
  DEBUG_CFLAGS="-DENABLE_DEBUG"
else
  DEBUG_CFLAGS=""
fi
AC_SUBST(DEBUG_CFLAGS)

AM_CONDITIONAL(WITH_PYTHON, [test "$WITH_PYTHON" = "yes"])

AM_CONDITIONAL(WITH_EXAMPLES, [test "$WITH_EXAMPLES" = "yes"])

AC_CONFIG_FILES(
  Makefile
  src/Makefile
  examples/Makefile
  gtk-vnc-1.0.pc
  gtk-vnc.spec
  )

AC_OUTPUT
