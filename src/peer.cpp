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

#include "peer.h"
#include <algorithm>
#include <cctype>
#include <rtc/global.hpp>
#include <sstream>
#include <thread>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace amber::rtc {
namespace {
std::atomic<size_t> active{0};
std::atomic<size_t> cleanupFailures{0};
std::once_flag backendRetained;
thread_local std::vector<std::weak_ptr<Peer>> requestPeers;

bool numeric_ip(const std::string& ip){
	unsigned char bytes[16];
	return inet_pton(AF_INET, ip.c_str(), bytes) == 1 || inet_pton(AF_INET6, ip.c_str(), bytes) == 1;
}
}

void retain_library(const void* address){
	// moved closed callbacks can outlive resetCallbacks
#ifdef _WIN32
	HMODULE module;
	if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
						   reinterpret_cast<LPCWSTR>(address), &module)){
		throw Error(11, "Unable to retain RTC library lifetime");
	}
#else
	Dl_info info{};
	if(!dladdr(address, &info) || !dlopen(info.dli_fname, RTLD_NOW | RTLD_NODELETE)){
		throw Error(11, "Unable to retain RTC library lifetime");
	}
#endif
}

namespace {
bool clean(const std::string& value){
	return std::none_of(value.begin(), value.end(), [](unsigned char c){ return c <= 32 || c == 127; });
}

void validate_bind(const ::rtc::Configuration& config){
	if(!config.bindAddress){
		return;
	}
	const auto& ip = *config.bindAddress;
	sockaddr_storage storage{};
	auto* v4 = reinterpret_cast<sockaddr_in*>(&storage);
	auto* v6 = reinterpret_cast<sockaddr_in6*>(&storage);
	int family = ip.find(':') == std::string::npos ? AF_INET : AF_INET6;
	if(family == AF_INET){
		v4->sin_family = AF_INET;
		inet_pton(AF_INET, ip.c_str(), &v4->sin_addr);
	} else{
		v6->sin6_family = AF_INET6;
		inet_pton(AF_INET6, ip.c_str(), &v6->sin6_addr);
	}
	const auto fd = socket(family, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
	if(fd == INVALID_SOCKET){
		throw Error(11, "Unable to validate bind address");
	}
#else
	if(fd < 0){
		throw Error(11, "Unable to validate bind address");
	}
#endif
	const int result =
		bind(fd, reinterpret_cast<sockaddr*>(&storage), family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6));
#ifdef _WIN32
	closesocket(fd);
#else
	::close(fd);
#endif
	if(result != 0){
		throw Error(1, "Bind address is unavailable");
	}
}
}

void validate_ip(const std::string& ip){
	if(!clean(ip) || ip.find('\0') != std::string::npos || !numeric_ip(ip)){
		throw Error(1, "Expected a numeric IP address");
	}
}

void validate_host(const std::string& host){
	if(host.empty() || host.size() > 253 || !clean(host) || host.find('\0') != std::string::npos){
		throw Error(1, "Invalid ICE host");
	}
	if(numeric_ip(host)){
		return;
	}
	std::istringstream input(host);
	std::string label;
	while(std::getline(input, label, '.')){
		if(label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-' ||
		   !std::all_of(label.begin(), label.end(), [](unsigned char c){
			   return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
		   })){
			throw Error(1, "Invalid ICE host");
		}
	}
}

void validate_candidate(const std::string& candidate, const std::string& mid){
	if(candidate.empty() || candidate.size() > 4096 || mid.empty() || mid.size() > 256 ||
	   candidate.find_first_of(std::string("\0\r\n", 3)) != std::string::npos || !clean(mid)){
		throw Error(1, "Invalid ICE candidate");
	}
	std::string text = candidate;
	if(text.starts_with("a=")){
		text.erase(0, 2);
	}
	if(!text.starts_with("candidate:")){
		throw Error(1, "Invalid ICE candidate");
	}
	std::istringstream input(text.substr(10));
	std::string foundation, component, transport, priority, address, port, typ, type;
	if(!(input >> foundation >> component >> transport >> priority >> address >> port >> typ >> type)){
		throw Error(1, "Invalid ICE candidate");
	}
	auto decimal = [](const std::string& s, uint64_t maximum){
		if(s.empty() || s.size() > 10 ||
		   !std::all_of(s.begin(), s.end(), [](unsigned char c){ return c >= '0' && c <= '9'; })){
			return false;
		}
		return std::stoull(s) <= maximum;
	};
	if(foundation.size() > 32 || !clean(foundation) || component != "1" || (transport != "UDP" && transport != "udp") ||
	   !decimal(priority, UINT32_MAX) || !numeric_ip(address) || !decimal(port, 65535) || std::stoul(port) == 0 ||
	   typ != "typ" || (type != "host" && type != "srflx" && type != "prflx" && type != "relay")){
		throw Error(1, "Unsupported or invalid ICE candidate");
	}
	try{
		::rtc::Candidate parsed(candidate, mid);
	} catch(...){
		throw Error(1, "Invalid ICE candidate");
	}
}

void validate_options(const Options& o){
	if(o.receiveBytes < o.message || o.sendBytes < o.message){
		throw Error(1, "Queue byte limits must fit maxMessageSize");
	}
	if(o.config.enableIceTcp || o.config.enableIceUdpMux){
		throw Error(1, "ICE TCP and UDP mux are not supported by this backend");
	}
	if(o.config.portRangeBegin <= 1024 && o.config.portRangeBegin != 1024 && o.config.portRangeEnd == 65535){
		throw Error(1, "Backend cannot enforce this port range");
	}
	size_t stun = 0, turn = 0;
	for(const auto& server : o.config.iceServers){
		if(server.type == ::rtc::IceServer::Type::Stun){
			++stun;
		} else{
			++turn;
			if(server.relayType != ::rtc::IceServer::RelayType::TurnUdp){
				throw Error(1, "Backend supports UDP TURN only");
			}
		}
	}
	if(stun > 1 || turn > 2){
		throw Error(1, "Backend supports one STUN and two TURN servers");
	}
}

std::shared_ptr<Peer> Peer::create(const Options& options){
	validate_options(options);
	std::call_once(backendRetained, [] { retain_library(reinterpret_cast<const void*>(&::rtc::Preload)); });
	auto self = std::make_shared<Peer>();
	self->options = options;
	auto config = options.config;
	config.disableAutoNegotiation = true;
	config.maxMessageSize = options.message;
	// peer construction acquires the backend token before the bind probe on windows
	self->backend = std::make_shared<::rtc::PeerConnection>(config);
	validate_bind(config);
	self->candidates.reserve(64);
	std::weak_ptr<Peer> weak = self;
	self->backend->onStateChange([weak](auto state) noexcept {
		if(auto p = weak.lock()){
			std::lock_guard lock(p->mutex);
			if(p->closed){
				return;
			}
			if(p->candidateOverflow && state != ::rtc::PeerConnection::State::Closed){
				return;
			}
			p->state = state;
			if(state == ::rtc::PeerConnection::State::Failed || state == ::rtc::PeerConnection::State::Disconnected){
				p->failure = state;
			}
		}
	});
	self->backend->onGatheringStateChange([weak](auto state) noexcept {
		if(auto p = weak.lock()){
			std::lock_guard lock(p->mutex);
			if(!p->closed){
				p->gathering = state;
			}
		}
	});
	self->backend->onLocalCandidate([weak](auto candidate) noexcept {
		if(auto p = weak.lock()){
			try{
				auto text = candidate.candidate(), mid = candidate.mid();
				std::lock_guard lock(p->mutex);
				if(p->closed){
					return;
				}
				if(p->candidates.size() < 64){
					p->candidates.emplace_back(std::move(text), std::move(mid));
				} else{
					p->candidateOverflow = true;
					p->state = ::rtc::PeerConnection::State::Failed;
					p->failure = p->state;
				}
			} catch(...){
				std::lock_guard lock(p->mutex);
				if(p->closed){
					return;
				}
				p->candidateOverflow = true;
				p->state = ::rtc::PeerConnection::State::Failed;
				p->failure = p->state;
			}
		}
	});
	// incoming channels are closed because the payload api is unavailable
	self->backend->onDataChannel([weak](auto channel) noexcept {
		try{
			channel->close();
		} catch(...){
			if(auto p = weak.lock()){
				std::lock_guard lock(p->mutex);
				if(!p->closed){
					p->state = ::rtc::PeerConnection::State::Failed;
					p->failure = p->state;
				}
			}
			++cleanupFailures;
		}
	});
	requestPeers.erase(std::remove_if(requestPeers.begin(), requestPeers.end(), [](auto& p){ return p.expired(); }),
					   requestPeers.end());
	requestPeers.emplace_back(self);
	++active;
	self->admitted = true;
	return self;
}

Peer::~Peer(){
	close_safely();
}

void Peer::close_safely() noexcept {
	try{
		close();
	} catch(...){
		++cleanupFailures;
	}
}

void Peer::require_open() const {
	std::lock_guard lock(mutex);
	if(closed || state == ::rtc::PeerConnection::State::Closed){
		throw Error(2, "Peer is closed");
	}
	if(candidateOverflow){
		throw Error(7, "Local candidate queue limit exceeded");
	}
}

void Peer::snapshot(){
	std::lock_guard operation(operations);
	if(closed || !backend){
		return;
	}
	auto description = backend->localDescription();
	auto fp = backend->remoteFingerprint();
	auto local = backend->localAddress(), remote = backend->remoteAddress();
	auto bytesSent = backend->bytesSent(), bytesReceived = backend->bytesReceived();
	auto currentRtt = backend->rtt();
	std::lock_guard lock(mutex);
	if(description){
		localDescription = std::string(*description);
	}
	if(!fp.value.empty() && fp.algorithm == ::rtc::CertificateFingerprint::Algorithm::Sha256){
		fingerprint = fp.value;
	}
	if(local){
		localAddress = std::move(local);
	}
	if(remote){
		remoteAddress = std::move(remote);
	}
	sent = (std::max)(sent, bytesSent);
	received = (std::max)(received, bytesReceived);
	if(currentRtt){
		rtt = currentRtt;
	}
}

namespace {
bool digits(const std::string& value){
	return !value.empty() &&
		   std::all_of(value.begin(), value.end(), [](unsigned char c){ return c >= '0' && c <= '9'; });
}

size_t validate_sdp(const std::string& sdp){
	if(sdp.empty() || sdp.find('\0') != std::string::npos || !sdp.starts_with("v=0")){
		throw Error(4, "Invalid remote description");
	}
	size_t count = 0;
	bool origin = false, session = false, timing = false, application = false, fingerprintLine = false;
	std::istringstream input(sdp);
	std::string line;
	bool firstLine = true;
	while(std::getline(input, line)){
		if(!line.empty() && line.back() == '\r'){
			line.pop_back();
		}
		if(firstLine && line != "v=0"){
			throw Error(4, "Invalid SDP version");
		}
		firstLine = false;
		if(line.find('\r') != std::string::npos){
			throw Error(4, "Invalid remote description");
		}
		if(!line.empty() && (line.size() < 2 || line[1] != '=')){
			throw Error(4, "Invalid SDP field");
		}
		if(line.starts_with("o=")){
			std::istringstream fields(line.substr(2));
			std::string user, id, revision, network, family, address, extra;
			if(!(fields >> user >> id >> revision >> network >> family >> address) || fields >> extra ||
			   network != "IN" || (family != "IP4" && family != "IP6")){
				throw Error(4, "Invalid SDP origin");
			}
			if(!digits(id) || !digits(revision)){
				throw Error(4, "Invalid SDP origin identifiers");
			}
		}
		if(line.starts_with("t=")){
			std::istringstream fields(line.substr(2));
			std::string start, end, extra;
			if(!(fields >> start >> end) || fields >> extra || !digits(start) || !digits(end)){
				throw Error(4, "Invalid SDP timing");
			}
		}
		origin |= line.starts_with("o=");
		session |= line.starts_with("s=");
		timing |= line.starts_with("t=");
		if(line.starts_with("m=")){
			if(!line.starts_with("m=application ")){
				throw Error(4, "Only application SDP is supported");
			}
			if(application){
				throw Error(4, "Multiple application sections are unsupported");
			}
			application = true;
		}
		if(line.starts_with("a=fingerprint:")){
			if(!line.starts_with("a=fingerprint:sha-256 ")){
				throw Error(4, "Only SHA-256 fingerprints are supported");
			}
			const auto value = line.substr(22);
			if(value.size() != 95){
				throw Error(4, "Invalid fingerprint");
			}
			for(size_t i = 0; i < value.size(); ++i){
				if(i % 3 == 2 ? value[i] != ':' : !std::isxdigit(static_cast<unsigned char>(value[i]))){
					throw Error(4, "Invalid fingerprint");
				}
			}
			fingerprintLine = true;
		}
		if(line.starts_with("a=candidate:")){
			if(++count > 64){
				throw Error(7, "Remote candidate limit exceeded");
			}
			try{
				validate_candidate(line, "0");
			} catch(...){
				throw Error(4, "Invalid SDP candidate");
			}
		}
	}
	if(!origin || !session || !timing || !application || !fingerprintLine){
		throw Error(4, "Incomplete remote description");
	}
	return count;
}
}

void Peer::remote(const std::string& sdp, bool offer){
	std::lock_guard operation(operations);
	require_open();
	using Signaling = ::rtc::PeerConnection::SignalingState;
	if((offer && (backend->signalingState() != Signaling::Stable || backend->remoteDescription())) ||
	   (!offer && backend->signalingState() != Signaling::HaveLocalOffer)){
		throw Error(3, "No matching signaling transition");
	}
	if(sdp.size() > options.description){
		throw Error(5, "Remote description exceeds configured limit");
	}
	const size_t count = validate_sdp(sdp);
	std::optional<::rtc::Description> description;
	try{
		description.emplace(sdp, offer ? ::rtc::Description::Type::Offer : ::rtc::Description::Type::Answer);
		if(!description->hasApplication() || !description->iceUfrag() || !description->icePwd() ||
		   !description->fingerprint()){
			throw Error(4, "Incomplete remote description");
		}
	} catch(...){
		throw Error(4, "Invalid remote description");
	}
	try{
		backend->setRemoteDescription(std::move(*description));
		remoteCandidates = count;
		if(offer){
			backend->setLocalDescription(::rtc::Description::Type::Answer);
		}
	} catch(const std::invalid_argument&){
		throw Error(4, "Backend rejected remote description");
	} catch(...){
		close();
		throw Error(11, "Negotiation failed");
	}
}

void Peer::add(const std::string& candidate, const std::string& mid){
	std::lock_guard operation(operations);
	require_open();
	if(!backend->remoteDescription()){
		throw Error(3, "Remote description is required");
	}
	if(remoteCandidates >= 64){
		throw Error(7, "Remote candidate limit exceeded");
	}
	validate_candidate(candidate, mid);
	try{
		backend->addRemoteCandidate(::rtc::Candidate(candidate, mid));
		++remoteCandidates;
	} catch(...){
		throw Error(1, "Backend rejected remote candidate");
	}
}

void Peer::close(){
	std::lock_guard operation(operations);
	{
		std::lock_guard lock(mutex);
		if(closed){
			return;
		}
	}
	bool snapshotFailed = false;
	if(backend){
		try{
			snapshot();
		} catch(...){
			snapshotFailed = true;
		}
	}
	{
		std::lock_guard lock(mutex);
		closed = true;
		state = ::rtc::PeerConnection::State::Closed;
		candidates.clear();
	}
	if(admitted){
		--active;
		admitted = false;
	}
	// backend close may synchronously deliver callbacks
	auto pc = std::move(backend);
	if(pc){
		pc->resetCallbacks();
		pc->close();
	}
	if(snapshotFailed){
		throw Error(11, "Final peer snapshot failed during close");
	}
}

size_t active_peers(){
	return active.load();
}

size_t cleanup_failures(){
	return cleanupFailures.load();
}

void shutdown_request() noexcept {
	for(auto& weak : requestPeers){
		if(auto p = weak.lock()){
			p->close_safely();
		}
	}
	requestPeers.clear();
}

bool shutdown_peers() noexcept {
	// backend globals may belong to another extension sharing the library
	return active.load() == 0 && cleanupFailures.load() == 0;
}
}
