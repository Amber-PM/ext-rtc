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

#pragma once

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "../../php_rtc.h"
#include "../peer/peer.h"
#include "../channel/channel.h"
#include <cstring>
extern "C" {
#include "Zend/zend_enum.h"
#include "Zend/zend_exceptions.h"
#include "ext/spl/spl_exceptions.h"
}

namespace amber::rtc::php {
extern zend_class_entry *exceptionClass, *optionsClass, *serverClass, *candidateClass, *peerClass, *stateClass,
	*gatheringClass, *channelClass, *channelOptionsClass;

struct CandidateValue{
	std::string candidate;
	std::string mid;
};

struct PeerValue{
	std::shared_ptr<Peer> peer;

	~PeerValue(){
		if(peer){
			peer->close_safely();
		}
	}
};

struct ChannelValue{
	std::shared_ptr<Channel> channel;
	~ChannelValue(){ if(channel){ channel->close_safely(); } }
};

struct Object{
	void* value;
	zend_object std;
};

inline Object* object(zend_object* obj){
	return reinterpret_cast<Object*>(reinterpret_cast<char*>(obj) - XtOffsetOf(Object, std));
}

// final classes select the payload type and matching handlers at registration
inline Options& options_value(zval* zv){
	return *static_cast<Options*>(object(Z_OBJ_P(zv))->value);
}

inline std::optional<::rtc::IceServer>& server_value(zval* zv){
	return *static_cast<std::optional<::rtc::IceServer>*>(object(Z_OBJ_P(zv))->value);
}

inline CandidateValue& candidate_value(zval* zv){
	return *static_cast<CandidateValue*>(object(Z_OBJ_P(zv))->value);
}

inline std::shared_ptr<Peer>& peer_value(zval* zv){
	return static_cast<PeerValue*>(object(Z_OBJ_P(zv))->value)->peer;
}

inline ChannelOptions& channel_options_value(zval* zv){
	return *static_cast<ChannelOptions*>(object(Z_OBJ_P(zv))->value);
}

inline std::shared_ptr<Channel>& channel_value(zval* zv){
	return static_cast<ChannelValue*>(object(Z_OBJ_P(zv))->value)->channel;
}

void translate_exception();
}

namespace amber::rtc {
PHP_METHOD(RtcValue, __construct);
PHP_METHOD(RtcValue, __clone);
PHP_METHOD(PeerConnectionOptions, create);
PHP_METHOD(PeerConnectionOptions, setMaxMessageSize);
PHP_METHOD(PeerConnectionOptions, setMaxReceiveQueueSize);
PHP_METHOD(PeerConnectionOptions, setMaxReceiveQueueMessages);
PHP_METHOD(PeerConnectionOptions, setMaxSendQueueSize);
PHP_METHOD(PeerConnectionOptions, setMaxRemoteDescriptionSize);
PHP_METHOD(PeerConnectionOptions, setMaxPendingDataChannels);
PHP_METHOD(PeerConnectionOptions, setIceTcpEnabled);
PHP_METHOD(PeerConnectionOptions, setIceUdpMuxEnabled);
PHP_METHOD(PeerConnectionOptions, setPortRange);
PHP_METHOD(PeerConnectionOptions, setBindAddress);
PHP_METHOD(PeerConnectionOptions, setIceServers);
PHP_METHOD(IceServer, stun);
PHP_METHOD(IceServer, turn);
PHP_METHOD(IceCandidate, __construct);
PHP_METHOD(IceCandidate, getCandidate);
PHP_METHOD(IceCandidate, getMid);
PHP_METHOD(PeerConnection, __construct);
PHP_METHOD(PeerConnection, setRemoteOffer);
PHP_METHOD(PeerConnection, setRemoteAnswer);
PHP_METHOD(PeerConnection, getState);
PHP_METHOD(PeerConnection, getFailureState);
PHP_METHOD(PeerConnection, getGatheringState);
PHP_METHOD(PeerConnection, getLocalDescription);
PHP_METHOD(PeerConnection, getRemoteFingerprint);
PHP_METHOD(PeerConnection, getLocalAddress);
PHP_METHOD(PeerConnection, getRemoteAddress);
PHP_METHOD(PeerConnection, pollLocalCandidates);
PHP_METHOD(PeerConnection, addRemoteCandidate);
PHP_METHOD(PeerConnection, getStats);
PHP_METHOD(PeerConnection, close);
}

namespace amber::rtc {
PHP_METHOD(DataChannelOptions, create);
PHP_METHOD(DataChannelOptions, setUnordered);
PHP_METHOD(DataChannelOptions, setMaxRetransmits);
PHP_METHOD(DataChannelOptions, setMaxPacketLifeTime);
PHP_METHOD(DataChannelOptions, setProtocol);
PHP_METHOD(DataChannel, getLabel);
PHP_METHOD(DataChannel, getProtocol);
PHP_METHOD(DataChannel, isOpen);
PHP_METHOD(DataChannel, isClosed);
PHP_METHOD(DataChannel, isUnordered);
PHP_METHOD(DataChannel, getMaxRetransmits);
PHP_METHOD(DataChannel, getMaxPacketLifeTime);
PHP_METHOD(DataChannel, getMaxMessageSize);
PHP_METHOD(DataChannel, getBufferedAmount);
PHP_METHOD(DataChannel, getAvailableAmount);
PHP_METHOD(DataChannel, getQueuedMessageCount);
PHP_METHOD(DataChannel, send);
PHP_METHOD(DataChannel, peek);
PHP_METHOD(DataChannel, receive);
PHP_METHOD(DataChannel, close);
PHP_METHOD(PeerConnection, createDataChannel);
PHP_METHOD(PeerConnection, pollDataChannels);
}
