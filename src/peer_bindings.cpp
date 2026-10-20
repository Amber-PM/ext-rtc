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
std::once_flag moduleRetained;

std::shared_ptr<Peer> get_peer(zval* zv){
	auto peer = peer_value(zv);
	if(!peer){
		throw Error(3, "Peer constructor has not completed");
	}
	return peer;
}

zend_long counter(size_t n){
	return n > static_cast<size_t>(ZEND_LONG_MAX) ? ZEND_LONG_MAX : static_cast<zend_long>(n);
}

void set_remote(zval* object, const char* sdp, size_t size, bool offer){
	auto peer = get_peer(object);
	peer->require_open();
	// reject oversized input before copying into native storage
	if(size > peer->options.description){
		throw Error(5, "Remote description exceeds configured limit");
	}
	peer->remote(std::string(sdp, size), offer);
}

void snapshot_string(zval* result, zval* object, std::optional<std::string> Peer::*field){
	auto peer = get_peer(object);
	peer->snapshot();
	std::optional<std::string> text;
	{
		std::lock_guard lock(peer->mutex);
		text = peer.get()->*field;
	}
	// zend allocation can bail out, so release the native lock first
	if(text){
		ZVAL_STRINGL(result, text->data(), text->size());
	} else{
		ZVAL_NULL(result);
	}
}

const char* state_name(::rtc::PeerConnection::State state){
	switch(state){
	case ::rtc::PeerConnection::State::New:
		return "NEW";
	case ::rtc::PeerConnection::State::Connecting:
		return "CONNECTING";
	case ::rtc::PeerConnection::State::Connected:
		return "CONNECTED";
	case ::rtc::PeerConnection::State::Disconnected:
		return "DISCONNECTED";
	case ::rtc::PeerConnection::State::Failed:
		return "FAILED";
	case ::rtc::PeerConnection::State::Closed:
		return "CLOSED";
	}
	return "FAILED";
}

void enum_result(zval* result, zend_class_entry* ce, const char* name){
	ZVAL_OBJ_COPY(result, zend_enum_get_case_cstr(ce, name));
}
}

PHP_METHOD(PeerConnection, __construct){
	zval* options;
	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_OBJECT_OF_CLASS(options, optionsClass)
	ZEND_PARSE_PARAMETERS_END();
	try{
		auto& peer = peer_value(ZEND_THIS);
		if(peer){
			throw Error(3, "Peer is already initialized");
		}
		std::call_once(moduleRetained, [] { retain_library(&rtc_module_entry); });
		peer = Peer::create(options_value(options));
	} catch(...){
		translate_exception();
		RETURN_THROWS();
	}
}

PHP_METHOD(PeerConnection, setRemoteOffer){
	char* sdp;
	size_t size;
	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_STRING(sdp, size)
	ZEND_PARSE_PARAMETERS_END();
	try{
		set_remote(ZEND_THIS, sdp, size, true);
	} catch(...){
		translate_exception();
		RETURN_THROWS();
	}
}

PHP_METHOD(PeerConnection, setRemoteAnswer){
	char* sdp;
	size_t size;
	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_STRING(sdp, size)
	ZEND_PARSE_PARAMETERS_END();
	try{
		set_remote(ZEND_THIS, sdp, size, false);
	} catch(...){
		translate_exception();
		RETURN_THROWS();
	}
}

PHP_METHOD(PeerConnection, getState){
	ZEND_PARSE_PARAMETERS_NONE();
	try{
		auto peer = get_peer(ZEND_THIS);
		const char* name;
		{
			std::lock_guard lock(peer->mutex);
			name = state_name(peer->state);
		}
		enum_result(return_value, stateClass, name);
	} catch(...){
		translate_exception();
		RETURN_THROWS();
	}
}

PHP_METHOD(PeerConnection, getFailureState){
	ZEND_PARSE_PARAMETERS_NONE();
	try{
		auto peer = get_peer(ZEND_THIS);
		const char* name = nullptr;
		{
			std::lock_guard lock(peer->mutex);
			if(peer->failure){
				name = state_name(*peer->failure);
			}
		}
		if(name){
			enum_result(return_value, stateClass, name);
		} else{
			RETURN_NULL();
		}
	} catch(...){
		translate_exception();
		RETURN_THROWS();
	}
}

PHP_METHOD(PeerConnection, getGatheringState){
	ZEND_PARSE_PARAMETERS_NONE();
	try{
		auto peer = get_peer(ZEND_THIS);
		const char* name;
		{
			std::lock_guard lock(peer->mutex);
			name = peer->gathering == ::rtc::PeerConnection::GatheringState::Complete	   ? "COMPLETE"
				   : peer->gathering == ::rtc::PeerConnection::GatheringState::InProgress ? "IN_PROGRESS"
																					   : "NEW";
		}
		enum_result(return_value, gatheringClass, name);
	} catch(...){
		translate_exception();
		RETURN_THROWS();
	}
}

PHP_METHOD(PeerConnection, getLocalDescription){
	ZEND_PARSE_PARAMETERS_NONE();
	try{
		snapshot_string(return_value, ZEND_THIS, &Peer::localDescription);
	} catch(...){
		translate_exception();
		RETURN_THROWS();
	}
}

PHP_METHOD(PeerConnection, getRemoteFingerprint){
	ZEND_PARSE_PARAMETERS_NONE();
	try{
		snapshot_string(return_value, ZEND_THIS, &Peer::fingerprint);
	} catch(...){
		translate_exception();
		RETURN_THROWS();
	}
}

PHP_METHOD(PeerConnection, getLocalAddress){
	ZEND_PARSE_PARAMETERS_NONE();
	try{
		snapshot_string(return_value, ZEND_THIS, &Peer::localAddress);
	} catch(...){
		translate_exception();
		RETURN_THROWS();
	}
}

PHP_METHOD(PeerConnection, getRemoteAddress){
	ZEND_PARSE_PARAMETERS_NONE();
	try{
		snapshot_string(return_value, ZEND_THIS, &Peer::remoteAddress);
	} catch(...){
		translate_exception();
		RETURN_THROWS();
	}
}

PHP_METHOD(PeerConnection, pollLocalCandidates){
	ZEND_PARSE_PARAMETERS_NONE();
	try{
		auto peer = get_peer(ZEND_THIS);
		std::vector<std::pair<std::string, std::string>> items;
		{
			std::lock_guard lock(peer->mutex);
			if(!peer->closed && peer->state != ::rtc::PeerConnection::State::Closed){
				if(peer->candidateOverflow){
					throw Error(7, "Local candidate queue limit exceeded");
				}
				items.swap(peer->candidates);
			}
		}
		array_init_size(return_value, items.size());
		for(auto& [candidate, mid] : items){
			zval item;
			object_init_ex(&item, candidateClass);
			candidate_value(&item).candidate = std::move(candidate);
			candidate_value(&item).mid = std::move(mid);
			add_next_index_zval(return_value, &item);
		}
	} catch(...){
		translate_exception();
		RETURN_THROWS();
	}
}

PHP_METHOD(PeerConnection, addRemoteCandidate){
	zval* candidate;
	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_OBJECT_OF_CLASS(candidate, candidateClass)
	ZEND_PARSE_PARAMETERS_END();
	try{
		auto peer = get_peer(ZEND_THIS);
		auto& c = candidate_value(candidate);
		peer->add(c.candidate, c.mid);
	} catch(...){
		translate_exception();
		RETURN_THROWS();
	}
}

PHP_METHOD(PeerConnection, getStats){
	ZEND_PARSE_PARAMETERS_NONE();
	try{
		auto peer = get_peer(ZEND_THIS);
		peer->snapshot();
		size_t sent, received;
		std::optional<std::chrono::milliseconds> rtt;
		{
			std::lock_guard lock(peer->mutex);
			sent = peer->sent;
			received = peer->received;
			rtt = peer->rtt;
		}
		array_init_size(return_value, 8);
		if(rtt){
			add_assoc_long(return_value, "rttMs", counter(static_cast<size_t>(std::max<int64_t>(0, rtt->count()))));
		} else{
			add_assoc_null(return_value, "rttMs");
		}
		add_assoc_long(return_value, "bytesSent", counter(sent));
		add_assoc_long(return_value, "bytesReceived", counter(received));
		for(const char* key : {"receiveQueueBytes", "receiveQueueMessages", "sendQueueBytes", "receiveOverflowCount",
							   "sendRejectedCount"}){
			add_assoc_long(return_value, key, 0);
		}
	} catch(...){
		translate_exception();
		RETURN_THROWS();
	}
}

PHP_METHOD(PeerConnection, close){
	ZEND_PARSE_PARAMETERS_NONE();
	try{
		get_peer(ZEND_THIS)->close();
	} catch(...){
		translate_exception();
		RETURN_THROWS();
	}
}

}
