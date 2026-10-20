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
#include "runtime.h"
#include "peer.h"

namespace amber::rtc {

ZEND_BEGIN_ARG_INFO_EX(arginfo_rtc_private, 0, 0, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_rtc_string, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_rtc_int, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_rtc_array, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_rtc_void, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(RtcRuntime, __construct){
	ZEND_PARSE_PARAMETERS_NONE();
}

PHP_METHOD(RtcRuntime, __clone){
	ZEND_PARSE_PARAMETERS_NONE();
}

PHP_METHOD(RtcRuntime, getVersion){
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_STRING(PHP_RTC_VERSION);
}

PHP_METHOD(RtcRuntime, getApiVersion){
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_LONG(PHP_RTC_API_VERSION);
}

PHP_METHOD(RtcRuntime, getCapabilities){
	ZEND_PARSE_PARAMETERS_NONE();
	array_init_size(return_value, 6);
	add_assoc_bool(return_value, "nativeBootstrap", true);
	add_assoc_bool(return_value, "peerConnection", true);
	add_assoc_bool(return_value, "dataChannels", false);
	add_assoc_bool(return_value, "trickleIce", true);
	add_assoc_bool(return_value, "observedRemoteFingerprint", true);
	add_assoc_bool(return_value, "media", false);
}

PHP_METHOD(RtcRuntime, getStats){
	ZEND_PARSE_PARAMETERS_NONE();
	array_init_size(return_value, 5);
	add_assoc_long(return_value, "activePeers", static_cast<zend_long>(active_peers()));
	add_assoc_long(return_value, "activeChannels", 0);
	add_assoc_long(return_value, "managedBufferBytes", 0);
	add_assoc_long(return_value, "receiveOverflowCount", 0);
	add_assoc_long(return_value, "sendRejectedCount", 0);
}

static const zend_function_entry runtime_methods[] = {
	PHP_ME(RtcRuntime, __construct, arginfo_rtc_private, ZEND_ACC_PRIVATE)
	PHP_ME(RtcRuntime, __clone, arginfo_rtc_void, ZEND_ACC_PRIVATE)
	PHP_ME(RtcRuntime, getVersion, arginfo_rtc_string, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(RtcRuntime, getApiVersion, arginfo_rtc_int, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(RtcRuntime, getCapabilities, arginfo_rtc_array, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(RtcRuntime, getStats, arginfo_rtc_array, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_FE_END
};

void register_runtime(){
	zend_class_entry entry;
	INIT_NS_CLASS_ENTRY(entry, "amber\\rtc", "RtcRuntime", runtime_methods);
	zend_class_entry* registered = zend_register_internal_class(&entry);
	registered->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NOT_SERIALIZABLE | ZEND_ACC_NO_DYNAMIC_PROPERTIES;
}
}
