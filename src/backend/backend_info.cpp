/*
 *
 *  _____ __  _______   ____ _____ ____
 * | ____|\ \/ /_   _| |  _ \_   _/ ___|
 * |  _|   \  /  | |   | |_) || || |
 * | |___  /  \  | |   |  _ < | || |___
 * |_____|/_/\_\ |_|   |_| \_\|_| \____|
 *
 * Copyright (c) 2026 Amber-PM contributors
 * SPDX-License-Identifier: MIT
 *
 * https://github.com/Amber-PM/ext-rtc
 *
 */

#include "backend_info.h"
#include <rtc/global.hpp>
#include <rtc/datachannel.hpp>
#include <rtc/ext_rtc_backend.h>
#include <rtc/version.h>
#include <cstring>
#include <sstream>

#if RTC_ENABLE_MEDIA
#error "ext-rtc requires libdatachannel built with NO_MEDIA=ON"
#endif
#if RTC_ENABLE_WEBSOCKET
#error "ext-rtc requires libdatachannel built with NO_WEBSOCKET=ON"
#endif

namespace amber::rtc {
namespace {
bool (::rtc::DataChannel::*volatile lifetime_send)(const ::rtc::byte*, size_t, std::shared_ptr<void>) = &::rtc::DataChannel::send;
}
const char* backend_version() noexcept{
	return RTC_VERSION;
}

const char* backend_identity() noexcept{
	return ::rtcExtMessageLifetimeIdentity();
}

bool backend_probe(){
	const auto* identity = backend_identity();
	if(!lifetime_send || !identity || std::strcmp(identity, expected_backend_identity) != 0){ return false; }
	std::ostringstream output;
	::rtc::operator<<(output, ::rtc::LogLevel::Warning);
	return output.str() == "warning";
}
}
