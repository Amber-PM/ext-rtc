PHP_ARG_ENABLE([rtc], [whether to enable rtc], [AS_HELP_STRING([--enable-rtc], [Enable rtc bootstrap])], [no])
PHP_ARG_WITH([libdatachannel], [private libdatachannel prefix], [AS_HELP_STRING([--with-libdatachannel=DIR], [Pinned libdatachannel prefix])], [no], [no])

if test "$PHP_RTC" != "no"; then
  if test "$PHP_LIBDATACHANNEL" = "no" || test ! -f "$PHP_LIBDATACHANNEL/include/rtc/global.hpp"; then
    AC_MSG_ERROR([rtc requires --with-libdatachannel=PREFIX containing pinned headers])
  fi
  if test ! -f "$PHP_LIBDATACHANNEL/lib/libdatachannel.so"; then
    AC_MSG_ERROR([rtc requires linkable libdatachannel 0.24.6 with its exported probe symbol in PREFIX/lib])
  fi
  PHP_REQUIRE_CXX()
  AC_LANG_PUSH([C++])
  rtc_saved_CXXFLAGS="$CXXFLAGS"
  CXXFLAGS="$CXXFLAGS -std=c++20"
  AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[#include <concepts>]], [[static_assert(__cplusplus >= 202002L);]])], [],
    [AC_MSG_ERROR([rtc requires a C++20 compiler])])
  rtc_saved_CPPFLAGS="$CPPFLAGS"
  rtc_saved_LIBS="$LIBS"
  CPPFLAGS="$CPPFLAGS -I$PHP_LIBDATACHANNEL/include -DRTC_ENABLE_MEDIA=0 -DRTC_ENABLE_WEBSOCKET=0"
  LIBS="$PHP_LIBDATACHANNEL/lib/libdatachannel.so $LIBS"
  AC_MSG_CHECKING([for the pinned libdatachannel headers and exported probe symbol])
  AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <rtc/global.hpp>
#include <rtc/version.h>
#include <sstream>
static_assert(RTC_VERSION_MAJOR == 0 && RTC_VERSION_MINOR == 24 && RTC_VERSION_PATCH == 6);]],
    [[std::ostringstream output; rtc::operator<<(output, rtc::LogLevel::Warning);]])],
    [AC_MSG_RESULT([yes])], [AC_MSG_ERROR([rtc requires linkable libdatachannel 0.24.6 with its exported probe symbol])])
  CPPFLAGS="$rtc_saved_CPPFLAGS"
  LIBS="$rtc_saved_LIBS"
  CXXFLAGS="$rtc_saved_CXXFLAGS"
  AC_LANG_POP([C++])
  PHP_ADD_INCLUDE([$PHP_LIBDATACHANNEL/include])
  PHP_ADD_LIBPATH([$PHP_LIBDATACHANNEL/lib], [RTC_SHARED_LIBADD])
  RTC_SHARED_LIBADD="$RTC_SHARED_LIBADD -Wl,$PHP_LIBDATACHANNEL/lib/libdatachannel.so"
  PHP_ADD_LIBRARY([stdc++], [1], [RTC_SHARED_LIBADD])
  PHP_SUBST([RTC_SHARED_LIBADD])
  PHP_NEW_EXTENSION([rtc], [src/rtc.cpp src/backend_info.cpp src/runtime.cpp], [$ext_shared],, [-std=c++20 -DZEND_ENABLE_STATIC_TSRMLS_CACHE=1 -DRTC_ENABLE_MEDIA=0 -DRTC_ENABLE_WEBSOCKET=0], [cxx])
fi
