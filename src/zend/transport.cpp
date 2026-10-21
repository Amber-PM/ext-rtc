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

#include "transport.h"

namespace amber::rtc::php {
zend_class_entry *exceptionClass, *optionsClass, *serverClass, *candidateClass, *peerClass, *stateClass,
	*gatheringClass, *channelClass, *channelOptionsClass;

void translate_exception(){
	try{
		throw;
	} catch(const Error& error){
		zend_throw_exception(exceptionClass, error.what(), error.code);
	} catch(const std::bad_alloc&){
		zend_throw_exception(exceptionClass, "Native allocation failed", 10);
	} catch(...){
		zend_throw_exception(exceptionClass, "Native RTC operation failed", 11);
	}
}
}

namespace amber::rtc {
using namespace php;

namespace {
template <class T> zend_object_handlers handlers;

template <class T> zend_object* create_object(zend_class_entry* ce){
	auto* obj = static_cast<Object*>(zend_object_alloc(sizeof(Object), ce));
	obj->value = nullptr;
	zend_object_std_init(&obj->std, ce);
	object_properties_init(&obj->std, ce);
	obj->std.handlers = &handlers<T>;
	try{
		obj->value = new T;
	} catch(...){
		zend_error_noreturn(E_ERROR, "rtc native object allocation failed");
	}
	return &obj->std;
}

template <class T> void free_object(zend_object* obj){
	// peer payload closes on the php thread before releasing its native reference
	delete static_cast<T*>(object(obj)->value);
	zend_object_std_dtor(obj);
}

HashTable* debug_info(zend_object*, int* temporary){
	*temporary = 1;
	return zend_new_array(0);
}

template <class T> zend_class_entry* register_class(const char* name, const zend_function_entry* methods){
	auto& h = handlers<T>;
	std::memcpy(&h, zend_get_std_object_handlers(), sizeof(h));
	h.offset = XtOffsetOf(Object, std);
	h.free_obj = free_object<T>;
	h.clone_obj = nullptr;
	h.get_debug_info = debug_info;
	zend_class_entry entry;
	std::string qualified = "amber\\rtc\\";
	qualified += name;
	INIT_CLASS_ENTRY_EX(entry, qualified.c_str(), qualified.size(), methods);
	auto* registered = zend_register_internal_class(&entry);
	registered->create_object = create_object<T>;
	registered->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NOT_SERIALIZABLE | ZEND_ACC_NO_DYNAMIC_PROPERTIES;
	return registered;
}
}

#include "transport_arginfo.h"

void register_transport(){
	stateClass = zend_register_internal_enum("amber\\rtc\\ConnectionState", IS_UNDEF, nullptr);
	for(const char* name : {"NEW", "CONNECTING", "CONNECTED", "DISCONNECTED", "FAILED", "CLOSED"}){
		zend_enum_add_case_cstr(stateClass, name, nullptr);
	}
	gatheringClass = zend_register_internal_enum("amber\\rtc\\GatheringState", IS_UNDEF, nullptr);
	for(const char* name : {"NEW", "IN_PROGRESS", "COMPLETE"}){
		zend_enum_add_case_cstr(gatheringClass, name, nullptr);
	}
	zend_class_entry entry;
	INIT_NS_CLASS_ENTRY(entry, "amber\\rtc", "RtcException", nullptr);
	exceptionClass = zend_register_internal_class_ex(&entry, spl_ce_RuntimeException);
	exceptionClass->ce_flags |= ZEND_ACC_FINAL;
	const char* codes[] = {
		"INVALID_ARGUMENT", "CLOSED", "INVALID_STATE", "INVALID_SDP", "SDP_TOO_LARGE", "PAYLOAD_TOO_LARGE",
		"CANDIDATE_LIMIT", "CHANNEL_LIMIT", "RECEIVE_OVERFLOW", "BUFFER_BUDGET", "BACKEND_FAILURE", "SHUTDOWN_FAILURE"
	};
	for(size_t i = 0; i < 12; ++i){
		zend_declare_class_constant_long(exceptionClass, codes[i], strlen(codes[i]), i + 1);
	}
	optionsClass = register_class<Options>("PeerConnectionOptions", PeerConnectionOptions_methods);
	serverClass = register_class<std::optional<::rtc::IceServer>>("IceServer", IceServer_methods);
	candidateClass = register_class<CandidateValue>("IceCandidate", IceCandidate_methods);
	channelOptionsClass = register_class<ChannelOptions>("DataChannelOptions", DataChannelOptions_methods);
	channelClass = register_class<ChannelValue>("DataChannel", DataChannel_methods);
	peerClass = register_class<PeerValue>("PeerConnection", PeerConnection_methods);
}
}
