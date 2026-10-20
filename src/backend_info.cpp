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
#include <rtc/version.h>
#include <sstream>

#if RTC_ENABLE_MEDIA
#error "ext-rtc requires libdatachannel built with NO_MEDIA=ON"
#endif
#if RTC_ENABLE_WEBSOCKET
#error "ext-rtc requires libdatachannel built with NO_WEBSOCKET=ON"
#endif

namespace amber::rtc {
const char* backend_version() noexcept{
	return RTC_VERSION;
}

bool backend_probe(){
	std::ostringstream output;
	::rtc::operator<<(output, ::rtc::LogLevel::Warning);
	return output.str() == "warning";
}
}
