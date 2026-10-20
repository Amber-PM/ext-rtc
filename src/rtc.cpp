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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "../php_rtc.h"
#include "backend_info.h"
#include "runtime.h"
#include <exception>

extern "C" {
#include "ext/standard/info.h"
}

PHP_MINIT_FUNCTION(rtc){
	try{
		if(!amber::rtc::backend_probe()){
			php_error_docref(nullptr, E_WARNING, "libdatachannel linkage probe failed");
			return FAILURE;
		}
		amber::rtc::register_runtime();
		return SUCCESS;
	}catch(const std::exception&){
		php_error_docref(nullptr, E_WARNING, "libdatachannel linkage probe threw");
		return FAILURE;
	}catch(...){
		php_error_docref(nullptr, E_WARNING, "libdatachannel linkage probe failed unexpectedly");
		return FAILURE;
	}
}

PHP_MSHUTDOWN_FUNCTION(rtc){
	return SUCCESS;
}

PHP_MINFO_FUNCTION(rtc){
	php_info_print_table_start();
	php_info_print_table_header(2, "rtc support", "enabled");
	php_info_print_table_row(2, "Version", PHP_RTC_VERSION);
	php_info_print_table_row(2, "API version", "1");
	php_info_print_table_row(2, "Compiled libdatachannel version", amber::rtc::backend_version());
	php_info_print_table_row(2, "libdatachannel commit", PHP_RTC_BACKEND_COMMIT);
	php_info_print_table_row(2, "Media", "disabled (NO_MEDIA=ON)");
	php_info_print_table_row(2, "PHP API", ZEND_TOSTR(PHP_API_VERSION));
#ifdef ZTS
	php_info_print_table_row(2, "Thread safety", "ZTS");
#else
	php_info_print_table_row(2, "Thread safety", "NTS");
#endif
#ifdef PHP_WIN32
	php_info_print_table_row(2, "Platform", "Windows");
#else
	php_info_print_table_row(2, "Platform", "Linux");
#endif
	php_info_print_table_end();
}

zend_module_entry rtc_module_entry = {
	STANDARD_MODULE_HEADER,
	"rtc",
	nullptr,
	PHP_MINIT(rtc),
	PHP_MSHUTDOWN(rtc),
	nullptr,
	nullptr,
	PHP_MINFO(rtc),
	PHP_RTC_VERSION,
	STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_RTC
extern "C" {
ZEND_GET_MODULE(rtc)
}
#endif
