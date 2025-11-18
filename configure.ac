AC_INIT([ie], [1.0.0], [zdhdev@yahoo.com])
AM_INIT_AUTOMAKE([foreign subdir-objects])

AC_CONFIG_HEADERS([src/include/config.h])

AC_CONFIG_SRCDIR([src/main.c])

AC_PROG_CC
AC_PROG_INSTALL

# Check for libforge
AC_CHECK_LIB([forge], [main], [], [
    AC_MSG_ERROR([libforge not found. Please install libforge (https://github.com/malloc-nbytes/forge).])
])

: ${CFLAGS="-O2 -pipe -Wextra -pedantic"}

AC_DEFINE_UNQUOTED([PREFIX], ["$prefix"], [Installation prefix])
AC_DEFINE_UNQUOTED([COMPILE_FLAGS], ["$CFLAGS $CPPFLAGS"], [Compilation flags used])

AC_CONFIG_FILES([Makefile src/Makefile])
AC_OUTPUT
