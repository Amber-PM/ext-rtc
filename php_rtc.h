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

#ifndef PHP_RTC_H
#define PHP_RTC_H

extern "C" {
#include "php.h"
}

#if PHP_VERSION_ID < 80100
#error "ext-rtc requires PHP 8.1 or newer"
#endif
static_assert(sizeof(zend_long) == 8, "ext-rtc initial builds require a 64-bit PHP ABI");

#define PHP_RTC_VERSION "0.1.0-dev"
#define PHP_RTC_API_VERSION 1
#define PHP_RTC_BACKEND_COMMIT "6b1e2e620f1e37f0eafeee702eaea0043cb305fd"

extern zend_module_entry rtc_module_entry;
#define phpext_rtc_ptr &rtc_module_entry

#endif
