PHP_ARG_ENABLE([rtc], [whether to enable rtc], [AS_HELP_STRING([--enable-rtc], [Enable rtc bootstrap])], [no])
PHP_ARG_WITH([libdatachannel], [private libdatachannel prefix], [AS_HELP_STRING([--with-libdatachannel=DIR], [Pinned libdatachannel prefix])], [no], [no])

if test "$PHP_RTC" != "no"; then
  if test "$PHP_LIBDATACHANNEL" = "no" || test ! -f "$PHP_LIBDATACHANNEL/include/rtc/global.hpp"; then
    AC_MSG_ERROR([rtc requires --with-libdatachannel=PREFIX containing pinned headers])
  fi
  if test ! -f "$PHP_LIBDATACHANNEL/lib/libdatachannel.so"; then
    AC_MSG_ERROR([rtc requires linkable libdatachannel 0.24.6 with its message-lifetime overload and identity symbol in PREFIX/lib])
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
  AC_MSG_CHECKING([for the pinned libdatachannel headers and message-lifetime overload and identity symbol])
  AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <rtc/global.hpp>
#include <rtc/version.h>
#include <rtc/datachannel.hpp>
#include <rtc/ext_rtc_backend.h>
#include <memory>
#include <sstream>
static_assert(RTC_VERSION_MAJOR == 0 && RTC_VERSION_MINOR == 24 && RTC_VERSION_PATCH == 6);]],
    [[std::ostringstream output; rtc::operator<<(output, rtc::LogLevel::Warning);
using Send = bool (rtc::DataChannel::*)(const rtc::byte *, size_t, std::shared_ptr<void>);
Send volatile send = static_cast<Send>(&rtc::DataChannel::send);
return !send || !rtcExtMessageLifetimeIdentity();]])],
    [AC_MSG_RESULT([yes])], [AC_MSG_ERROR([rtc requires linkable libdatachannel 0.24.6 with its message-lifetime overload and identity symbol])])
  CPPFLAGS="$rtc_saved_CPPFLAGS"
  LIBS="$rtc_saved_LIBS"
  CXXFLAGS="$rtc_saved_CXXFLAGS"
  AC_LANG_POP([C++])
  PHP_ADD_INCLUDE([$PHP_LIBDATACHANNEL/include])
  PHP_ADD_LIBPATH([$PHP_LIBDATACHANNEL/lib], [RTC_SHARED_LIBADD])
  RTC_SHARED_LIBADD="$RTC_SHARED_LIBADD -Wl,$PHP_LIBDATACHANNEL/lib/libdatachannel.so"
  PHP_ADD_LIBRARY([stdc++], [1], [RTC_SHARED_LIBADD])
  PHP_ADD_LIBRARY([dl], [1], [RTC_SHARED_LIBADD])
  PHP_SUBST([RTC_SHARED_LIBADD])
  PHP_ADD_BUILD_DIR([$ext_builddir/src/backend], 1)
  PHP_ADD_BUILD_DIR([$ext_builddir/src/channel], 1)
  PHP_ADD_BUILD_DIR([$ext_builddir/src/core], 1)
  PHP_ADD_BUILD_DIR([$ext_builddir/src/peer], 1)
  PHP_ADD_BUILD_DIR([$ext_builddir/src/zend], 1)
  PHP_NEW_EXTENSION([rtc], [src/rtc.cpp src/core/buffers.cpp src/core/dispatcher.cpp src/peer/peer.cpp src/channel/channel.cpp src/channel/send.cpp src/channel/receive.cpp src/backend/backend_info.cpp src/zend/runtime.cpp src/zend/values.cpp src/zend/transport.cpp src/zend/peer_bindings.cpp src/zend/channel_bindings.cpp], [$ext_shared],, [-std=c++20 -DZEND_ENABLE_STATIC_TSRMLS_CACHE=1 -DRTC_ENABLE_MEDIA=0 -DRTC_ENABLE_WEBSOCKET=0], [cxx])
fi
