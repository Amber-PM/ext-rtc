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
size_t option_limit(zend_long n, uint64_t maximum){
	if(n <= 0 || static_cast<uint64_t>(n) > maximum){
		throw Error(1, "Invalid option limit");
	}
	return static_cast<size_t>(n);
}
}

PHP_METHOD(RtcValue, __construct){
	ZEND_PARSE_PARAMETERS_NONE();
}

PHP_METHOD(RtcValue, __clone){
	ZEND_PARSE_PARAMETERS_NONE();
}

PHP_METHOD(PeerConnectionOptions, create){
	ZEND_PARSE_PARAMETERS_NONE();
	object_init_ex(return_value, optionsClass);
}

PHP_METHOD(PeerConnectionOptions, setMaxMessageSize){
	zend_long n;
	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_LONG(n)
	ZEND_PARSE_PARAMETERS_END();
	try{
		options_value(ZEND_THIS).message = option_limit(n, 268435456);
		RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
	} catch(...){
		translate_exception();
		RETURN_THROWS();
	}
}

PHP_METHOD(PeerConnectionOptions, setMaxReceiveQueueSize){
	zend_long n;
	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_LONG(n)
	ZEND_PARSE_PARAMETERS_END();
	try{
		options_value(ZEND_THIS).receiveBytes = option_limit(n, ZEND_LONG_MAX);
		RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
	} catch(...){
		translate_exception();
		RETURN_THROWS();
	}
}

PHP_METHOD(PeerConnectionOptions, setMaxReceiveQueueMessages){
	zend_long n;
	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_LONG(n)
	ZEND_PARSE_PARAMETERS_END();
	try{
		options_value(ZEND_THIS).receiveMessages = option_limit(n, ZEND_LONG_MAX);
		RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
	} catch(...){
		translate_exception();
		RETURN_THROWS();
	}
}

PHP_METHOD(PeerConnectionOptions, setMaxSendQueueSize){
	zend_long n;
	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_LONG(n)
	ZEND_PARSE_PARAMETERS_END();
	try{
		options_value(ZEND_THIS).sendBytes = option_limit(n, ZEND_LONG_MAX);
		RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
	} catch(...){
		translate_exception();
		RETURN_THROWS();
	}
}

PHP_METHOD(PeerConnectionOptions, setMaxRemoteDescriptionSize){
	zend_long n;
	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_LONG(n)
	ZEND_PARSE_PARAMETERS_END();
	try{
		options_value(ZEND_THIS).description = option_limit(n, ZEND_LONG_MAX);
		RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
	} catch(...){
		translate_exception();
		RETURN_THROWS();
	}
}

PHP_METHOD(PeerConnectionOptions, setMaxPendingDataChannels){
	zend_long n;
	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_LONG(n)
	ZEND_PARSE_PARAMETERS_END();
	try{
		options_value(ZEND_THIS).pending = option_limit(n, 16);
		RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
	} catch(...){
		translate_exception();
		RETURN_THROWS();
	}
}

PHP_METHOD(PeerConnectionOptions, setIceTcpEnabled){
	bool enabled;
	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_BOOL(enabled)
	ZEND_PARSE_PARAMETERS_END();
	options_value(ZEND_THIS).config.enableIceTcp = enabled;
	RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

PHP_METHOD(PeerConnectionOptions, setIceUdpMuxEnabled){
	bool enabled;
	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_BOOL(enabled)
	ZEND_PARSE_PARAMETERS_END();
	options_value(ZEND_THIS).config.enableIceUdpMux = enabled;
	RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

PHP_METHOD(PeerConnectionOptions, setPortRange){
	zend_long first, last;
	ZEND_PARSE_PARAMETERS_START(2, 2)
	Z_PARAM_LONG(first)
	Z_PARAM_LONG(last)
	ZEND_PARSE_PARAMETERS_END();
	try{
		if(first < 1 || last < first || last > 65535){
			throw Error(1, "Invalid port range");
		}
		auto& c = options_value(ZEND_THIS).config;
		c.portRangeBegin = static_cast<uint16_t>(first);
		c.portRangeEnd = static_cast<uint16_t>(last);
		RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
	} catch(...){
		translate_exception();
		RETURN_THROWS();
	}
}

PHP_METHOD(PeerConnectionOptions, setBindAddress){
	char* ip;
	size_t length;
	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_STRING_OR_NULL(ip, length)
	ZEND_PARSE_PARAMETERS_END();
	try{
		std::optional<std::string> address;
		if(ip){
			address = std::string(ip, length);
			validate_ip(*address);
		}
		options_value(ZEND_THIS).config.bindAddress = std::move(address);
		RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
	} catch(...){
		translate_exception();
		RETURN_THROWS();
	}
}

PHP_METHOD(PeerConnectionOptions, setIceServers){
	zval* args;
	int count;
	ZEND_PARSE_PARAMETERS_START(0, -1)
	Z_PARAM_VARIADIC('*', args, count)
	ZEND_PARSE_PARAMETERS_END();
	try{
		if(count > 16){
			throw Error(1, "Too many ICE servers");
		}
		std::vector<::rtc::IceServer> servers;
		for(int i = 0; i < count; ++i){
			if(Z_TYPE(args[i]) != IS_OBJECT || Z_OBJCE(args[i]) != serverClass){
				zend_argument_type_error(i + 1, "must be of type amber\\rtc\\IceServer");
				RETURN_THROWS();
			}
			auto& server = server_value(&args[i]);
			if(!server){
				throw Error(1, "Uninitialized ICE server");
			}
			servers.push_back(*server);
		}
		options_value(ZEND_THIS).config.iceServers = std::move(servers);
		RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
	} catch(...){
		translate_exception();
		RETURN_THROWS();
	}
}

PHP_METHOD(IceServer, stun){
	char* host;
	size_t length;
	zend_long port = 3478;
	ZEND_PARSE_PARAMETERS_START(1, 2)
	Z_PARAM_STRING(host, length)
	Z_PARAM_OPTIONAL
	Z_PARAM_LONG(port)
	ZEND_PARSE_PARAMETERS_END();
	try{
		std::string hostname(host, length);
		validate_host(hostname);
		if(port < 1 || port > 65535){
			throw Error(1, "Invalid ICE port");
		}
		::rtc::IceServer server(hostname, static_cast<uint16_t>(port));
		object_init_ex(return_value, serverClass);
		server_value(return_value) = std::move(server);
	} catch(...){
		translate_exception();
		RETURN_THROWS();
	}
}

PHP_METHOD(IceServer, turn){
	char *host, *username, *password, *transport = nullptr;
	size_t hostLen, userLen, passLen, transportLen = 0;
	zend_long port;
	ZEND_PARSE_PARAMETERS_START(4, 5)
	Z_PARAM_STRING(host, hostLen)
	Z_PARAM_LONG(port)
	Z_PARAM_STRING(username, userLen)
	Z_PARAM_STRING(password, passLen)
	Z_PARAM_OPTIONAL
	Z_PARAM_STRING(transport, transportLen)
	ZEND_PARSE_PARAMETERS_END();
	try{
		std::string hostname(host, hostLen), user(username, userLen), pass(password, passLen),
			mode = transport ? std::string(transport, transportLen) : "udp";
		validate_host(hostname);
		if(port < 1 || port > 65535 || userLen > 1024 || passLen > 1024 || user.find('\0') != std::string::npos ||
		   pass.find('\0') != std::string::npos){
			throw Error(1, "Invalid TURN configuration");
		}
		auto type = ::rtc::IceServer::RelayType::TurnUdp;
		if(mode == "tcp"){
			type = ::rtc::IceServer::RelayType::TurnTcp;
		} else if(mode == "tls"){
			type = ::rtc::IceServer::RelayType::TurnTls;
		} else if(mode != "udp"){
			throw Error(1, "Invalid TURN transport");
		}
		::rtc::IceServer server(hostname, static_cast<uint16_t>(port), user, pass, type);
		object_init_ex(return_value, serverClass);
		server_value(return_value) = std::move(server);
	} catch(...){
		translate_exception();
		RETURN_THROWS();
	}
}

PHP_METHOD(IceCandidate, __construct){
	char *candidate, *mid;
	size_t length, midLen;
	ZEND_PARSE_PARAMETERS_START(2, 2)
	Z_PARAM_STRING(candidate, length)
	Z_PARAM_STRING(mid, midLen)
	ZEND_PARSE_PARAMETERS_END();
	try{
		auto& v = candidate_value(ZEND_THIS);
		if(!v.candidate.empty()){
			throw Error(3, "ICE candidate is immutable");
		}
		std::string c(candidate, length), m(mid, midLen);
		validate_candidate(c, m);
		v.candidate = std::move(c);
		v.mid = std::move(m);
	} catch(...){
		translate_exception();
		RETURN_THROWS();
	}
}

PHP_METHOD(IceCandidate, getCandidate){
	ZEND_PARSE_PARAMETERS_NONE();
	const auto& s = candidate_value(ZEND_THIS).candidate;
	RETURN_STRINGL(s.data(), s.size());
}

PHP_METHOD(IceCandidate, getMid){
	ZEND_PARSE_PARAMETERS_NONE();
	const auto& s = candidate_value(ZEND_THIS).mid;
	RETURN_STRINGL(s.data(), s.size());
}
}
