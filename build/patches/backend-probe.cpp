// SPDX-License-Identifier: MIT
#include <rtc/datachannel.hpp>
#include <rtc/ext_rtc_backend.h>
#include <rtc/version.h>
#include <cstring>
#include <memory>
#ifdef _WIN32
#include <windows.h>
#endif
static_assert(RTC_VERSION_MAJOR == 0 && RTC_VERSION_MINOR == 24 && RTC_VERSION_PATCH == 6);
using Send = bool (rtc::DataChannel::*)(const rtc::byte *, size_t, std::shared_ptr<void>);
Send volatile lifetimeSend = static_cast<Send>(&rtc::DataChannel::send);
int main(int argc, char **argv) {
#ifdef _WIN32
 char loaded[MAX_PATH], expected[MAX_PATH];
 HMODULE module = GetModuleHandleA("datachannel.dll");
 DWORD length = module ? GetModuleFileNameA(module, loaded, MAX_PATH) : 0;
 DWORD expectedLength = argc == 2 ? GetFullPathNameA(argv[1], MAX_PATH, expected, nullptr) : 0;
 if (!length || length >= MAX_PATH || !expectedLength || expectedLength >= MAX_PATH || _stricmp(loaded, expected) != 0) return 2;
#endif
 return lifetimeSend == nullptr || std::strcmp(rtcExtMessageLifetimeIdentity(), EXT_RTC_BACKEND_IDENTITY) != 0;
}
