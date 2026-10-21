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

namespace amber::rtc {
using namespace php;
namespace {
Channel& get_channel(zval* value){
	auto& channel = channel_value(value);
	if(!channel){ throw Error(3, "Channel is not initialized"); }
	return *channel;
}
zend_long channel_counter(size_t value){
	return value > static_cast<size_t>(ZEND_LONG_MAX) ? ZEND_LONG_MAX : static_cast<zend_long>(value);
}
void read_channel(zval* result, zval* object, bool consume){
	auto& channel = get_channel(object);
	auto size = channel.read_size();
	if(!size){ ZVAL_NULL(result); return; }
	// the return zval roots the allocation before the native copy can fail
	ZVAL_STR(result, zend_string_alloc(*size, false));
	try{
		if(!channel.copy_front(Z_STRVAL_P(result), *size, consume)){
			zval_ptr_dtor(result);
			ZVAL_NULL(result);
		} else{
			Z_STRVAL_P(result)[*size] = '\0';
		}
	} catch(...){
		zval_ptr_dtor(result);
		ZVAL_NULL(result);
		throw;
	}
}
}

PHP_METHOD(DataChannelOptions, create){
	ZEND_PARSE_PARAMETERS_NONE();
	object_init_ex(return_value, channelOptionsClass);
}

PHP_METHOD(DataChannelOptions, setUnordered){
	bool enabled;
	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_BOOL(enabled)
	ZEND_PARSE_PARAMETERS_END();
	channel_options_value(ZEND_THIS).reliability.unordered = enabled;
	RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

PHP_METHOD(DataChannelOptions, setMaxRetransmits){
	zend_long value;
	bool isNull;
	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_LONG_OR_NULL(value, isNull)
	ZEND_PARSE_PARAMETERS_END();
	try{
		auto& reliability = channel_options_value(ZEND_THIS).reliability;
		if(!isNull && (value < 0 || value > 65535 || reliability.maxPacketLifeTime)){
			throw Error(1, "Invalid channel reliability");
		}
		if(isNull){ reliability.maxRetransmits.reset(); } else{ reliability.maxRetransmits = static_cast<unsigned int>(value); }
		RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
	} catch(...){ translate_exception(); RETURN_THROWS(); }
}

PHP_METHOD(DataChannelOptions, setMaxPacketLifeTime){
	zend_long value;
	bool isNull;
	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_LONG_OR_NULL(value, isNull)
	ZEND_PARSE_PARAMETERS_END();
	try{
		auto& reliability = channel_options_value(ZEND_THIS).reliability;
		if(!isNull && (value < 0 || value > 65535 || reliability.maxRetransmits)){
			throw Error(1, "Invalid channel reliability");
		}
		if(isNull){ reliability.maxPacketLifeTime.reset(); } else{ reliability.maxPacketLifeTime = std::chrono::milliseconds{value}; }
		RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
	} catch(...){ translate_exception(); RETURN_THROWS(); }
}

PHP_METHOD(DataChannelOptions, setProtocol){
	char* protocol;
	size_t size;
	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_STRING(protocol, size)
	ZEND_PARSE_PARAMETERS_END();
	try{
		validate_channel_text(std::string_view(protocol, size));
		channel_options_value(ZEND_THIS).protocol.assign(protocol, size);
		RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
	} catch(...){ translate_exception(); RETURN_THROWS(); }
}

PHP_METHOD(PeerConnection, createDataChannel){
	char* label;
	size_t size;
	zval* options = nullptr;
	ZEND_PARSE_PARAMETERS_START(1, 2)
	Z_PARAM_STRING(label, size)
	Z_PARAM_OPTIONAL
	Z_PARAM_OBJECT_OF_CLASS_OR_NULL(options, channelOptionsClass)
	ZEND_PARSE_PARAMETERS_END();
	// allocate the Zend owner before creating any native transport
	object_init_ex(return_value, channelClass);
	try{
		auto& peer = peer_value(ZEND_THIS);
		if(!peer){ throw Error(3, "Peer is not initialized"); }
		validate_channel_text(std::string_view(label, size));
		channel_value(return_value) = peer->create_channel(std::string(label, size),
			options ? channel_options_value(options) : ChannelOptions{});
	} catch(...){ translate_exception(); RETURN_THROWS(); }
}

PHP_METHOD(PeerConnection, pollDataChannels){
	ZEND_PARSE_PARAMETERS_NONE();
	try{
		auto& peer = peer_value(ZEND_THIS);
		if(!peer){ throw Error(3, "Peer is not initialized"); }
		size_t count;
		{
			std::lock_guard lock(peer->mutex);
			count = peer->closed || peer->state == ::rtc::PeerConnection::State::Closed ? 0 : peer->pendingChannels.size();
		}
		array_init_size(return_value, count);
		std::array<ChannelValue*, 16> targets{};
		for(size_t i = 0; i < count; ++i){
			zval item;
			object_init_ex(&item, channelClass);
			targets[i] = static_cast<ChannelValue*>(object(Z_OBJ(item))->value);
			add_next_index_zval(return_value, &item);
		}
		size_t taken = 0;
		{
			std::lock_guard lock(peer->mutex);
			if(!peer->closed && peer->state != ::rtc::PeerConnection::State::Closed){
				while(taken < count && !peer->pendingChannels.empty()){
					targets[taken++]->channel = std::move(peer->pendingChannels.front());
					peer->pendingChannels.erase(peer->pendingChannels.begin());
				}
			}
		}
		for(size_t i = taken; i < count; ++i){ zend_hash_index_del(Z_ARRVAL_P(return_value), i); }
	} catch(...){ translate_exception(); RETURN_THROWS(); }
}

PHP_METHOD(DataChannel, getLabel){
	ZEND_PARSE_PARAMETERS_NONE();
	try{
		auto& c = get_channel(ZEND_THIS);
		ZVAL_STRINGL(return_value, c.label.data(), c.label.size());
	} catch(...){ translate_exception(); RETURN_THROWS(); }
}

PHP_METHOD(DataChannel, getProtocol){
	ZEND_PARSE_PARAMETERS_NONE();
	try{
		auto& c = get_channel(ZEND_THIS);
		ZVAL_STRINGL(return_value, c.settings.protocol.data(), c.settings.protocol.size());
	} catch(...){ translate_exception(); RETURN_THROWS(); }
}

PHP_METHOD(DataChannel, isOpen){
	ZEND_PARSE_PARAMETERS_NONE();
	try{
		auto& c = get_channel(ZEND_THIS);
		ZVAL_BOOL(return_value, c.is_open());
	} catch(...){ translate_exception(); RETURN_THROWS(); }
}

PHP_METHOD(DataChannel, isClosed){
	ZEND_PARSE_PARAMETERS_NONE();
	try{
		auto& c = get_channel(ZEND_THIS);
		ZVAL_BOOL(return_value, c.is_closed());
	} catch(...){ translate_exception(); RETURN_THROWS(); }
}

PHP_METHOD(DataChannel, isUnordered){
	ZEND_PARSE_PARAMETERS_NONE();
	try{
		auto& c = get_channel(ZEND_THIS);
		ZVAL_BOOL(return_value, c.settings.reliability.unordered);
	} catch(...){ translate_exception(); RETURN_THROWS(); }
}

PHP_METHOD(DataChannel, getMaxRetransmits){
	ZEND_PARSE_PARAMETERS_NONE();
	try{
		auto& c = get_channel(ZEND_THIS);
		if(c.settings.reliability.maxRetransmits){ ZVAL_LONG(return_value, *c.settings.reliability.maxRetransmits); } else{ ZVAL_NULL(return_value); }
	} catch(...){ translate_exception(); RETURN_THROWS(); }
}

PHP_METHOD(DataChannel, getMaxPacketLifeTime){
	ZEND_PARSE_PARAMETERS_NONE();
	try{
		auto& c = get_channel(ZEND_THIS);
		if(c.settings.reliability.maxPacketLifeTime){ ZVAL_LONG(return_value, c.settings.reliability.maxPacketLifeTime->count()); } else{ ZVAL_NULL(return_value); }
	} catch(...){ translate_exception(); RETURN_THROWS(); }
}

PHP_METHOD(DataChannel, getMaxMessageSize){
	ZEND_PARSE_PARAMETERS_NONE();
	try{
		auto& c = get_channel(ZEND_THIS);
		ZVAL_LONG(return_value, channel_counter(c.capacity()));
	} catch(...){ translate_exception(); RETURN_THROWS(); }
}

PHP_METHOD(DataChannel, getBufferedAmount){
	ZEND_PARSE_PARAMETERS_NONE();
	try{
		auto& c = get_channel(ZEND_THIS);
		ZVAL_LONG(return_value, channel_counter(c.buffered_bytes()));
	} catch(...){ translate_exception(); RETURN_THROWS(); }
}

PHP_METHOD(DataChannel, getAvailableAmount){
	ZEND_PARSE_PARAMETERS_NONE();
	try{
		auto& c = get_channel(ZEND_THIS);
		ZVAL_LONG(return_value, channel_counter(c.available_bytes()));
	} catch(...){ translate_exception(); RETURN_THROWS(); }
}

PHP_METHOD(DataChannel, getQueuedMessageCount){
	ZEND_PARSE_PARAMETERS_NONE();
	try{
		auto& c = get_channel(ZEND_THIS);
		ZVAL_LONG(return_value, channel_counter(c.queued_messages()));
	} catch(...){ translate_exception(); RETURN_THROWS(); }
}

PHP_METHOD(DataChannel, close){
	ZEND_PARSE_PARAMETERS_NONE();
	try{
		auto& c = get_channel(ZEND_THIS);
		c.close();
	} catch(...){ translate_exception(); RETURN_THROWS(); }
}

PHP_METHOD(DataChannel, peek){
	ZEND_PARSE_PARAMETERS_NONE();
	try{ read_channel(return_value, ZEND_THIS, false); }
	catch(...){ translate_exception(); RETURN_THROWS(); }
}

PHP_METHOD(DataChannel, receive){
	ZEND_PARSE_PARAMETERS_NONE();
	try{ read_channel(return_value, ZEND_THIS, true); }
	catch(...){ translate_exception(); RETURN_THROWS(); }
}

PHP_METHOD(DataChannel, send){
	char* payload;
	size_t size;
	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_STRING(payload, size)
	ZEND_PARSE_PARAMETERS_END();
	try{
		RETURN_BOOL(get_channel(ZEND_THIS).send(std::string_view(payload, size)));
	} catch(...){ translate_exception(); RETURN_THROWS(); }
}
}
