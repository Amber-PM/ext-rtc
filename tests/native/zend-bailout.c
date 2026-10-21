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


#include "php.h"
#include "Zend/zend_alloc.h"

static ZEND_TLS zend_mm_heap *probe_heap;
static ZEND_TLS char probe_method[64];
static ZEND_TLS zend_long probe_at;
static ZEND_TLS zend_long probe_seen;

static void probe_install(void);

static void probe_restore(void){
	zend_mm_set_custom_handlers(probe_heap, NULL, NULL, NULL);
}

static void probe_check(void){
	zend_execute_data *frame = EG(current_execute_data);
	if(frame && frame->func->common.scope && frame->func->common.function_name &&
		strcmp(ZSTR_VAL(frame->func->common.scope->name), "amber\\rtc\\PeerConnection") == 0 &&
		strcmp(ZSTR_VAL(frame->func->common.function_name), probe_method) == 0){
		if(++probe_seen == probe_at){
			probe_method[0] = '\0';
			zend_error_noreturn(E_ERROR, "injected Zend allocation bailout");
		}
	}
}

static void *probe_malloc(size_t size ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC){
	probe_restore();
	probe_check();
	void *result = zend_mm_alloc(probe_heap, size);
	probe_install();
	return result;
}

static void probe_free(void *ptr ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC){
	probe_restore();
	zend_mm_free(probe_heap, ptr);
	probe_install();
}

static void *probe_realloc(void *ptr, size_t size ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC){
	probe_restore();
	probe_check();
	void *result = zend_mm_realloc(probe_heap, ptr, size);
	probe_install();
	return result;
}

static void probe_install(void){
	zend_mm_set_custom_handlers(probe_heap, probe_malloc, probe_free, probe_realloc);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_probe_arm, 0, 2, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, method, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, allocation, IS_LONG, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_probe_stop, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(rtc_probe_arm){
	char *method;
	size_t length;
	zend_long allocation;
	ZEND_PARSE_PARAMETERS_START(2, 2)
	Z_PARAM_STRING(method, length)
	Z_PARAM_LONG(allocation)
	ZEND_PARSE_PARAMETERS_END();
	probe_heap = zend_mm_get_heap();
	if(length >= sizeof(probe_method) || zend_mm_is_custom_heap(probe_heap)){
		zend_error_noreturn(E_ERROR, "unsupported probe allocator or method");
	}
	memcpy(probe_method, method, length);
	probe_method[length] = '\0';
	probe_at = allocation;
	probe_seen = 0;
	probe_install();
}

PHP_FUNCTION(rtc_probe_stop){
	ZEND_PARSE_PARAMETERS_NONE();
	probe_restore();
	probe_method[0] = '\0';
	RETURN_LONG(probe_seen);
}

PHP_RSHUTDOWN_FUNCTION(rtc_probe){
	if(probe_heap){ probe_restore(); }
	probe_heap = NULL;
	probe_method[0] = '\0';
	return SUCCESS;
}

static const zend_function_entry probe_functions[] = {
	PHP_FE(rtc_probe_arm, arginfo_probe_arm)
	PHP_FE(rtc_probe_stop, arginfo_probe_stop)
	PHP_FE_END
};

zend_module_entry rtc_probe_module_entry = {
	STANDARD_MODULE_HEADER, "rtc_probe", probe_functions,
	NULL, NULL, NULL, PHP_RSHUTDOWN(rtc_probe), NULL, "1", STANDARD_MODULE_PROPERTIES
};
ZEND_GET_MODULE(rtc_probe)
