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

#include <atomic>
#include <mutex>
#include <rtc/peerconnection.hpp>
#include <stdexcept>

namespace amber::rtc {

struct Error : std::runtime_error{
	int code;

	Error(int code, const char* message) : std::runtime_error(message), code(code){}
};

struct Options{
	size_t message = 262144;
	size_t receiveBytes = 4194304;
	size_t receiveMessages = 256;
	size_t sendBytes = 1048576;
	size_t description = 65536;
	size_t pending = 2;
	::rtc::Configuration config;
};

void validate_host(const std::string& host);
void validate_ip(const std::string& ip);
void validate_candidate(const std::string& candidate, const std::string& mid);
void validate_options(const Options& options);

struct Peer : std::enable_shared_from_this<Peer>{
	static std::shared_ptr<Peer> create(const Options& options);
	~Peer();
	void remote(const std::string& sdp, bool offer);
	void add(const std::string& candidate, const std::string& mid);
	void snapshot();
	void close();
	void close_safely() noexcept;
	void require_open() const;
	Options options;
	std::shared_ptr<::rtc::PeerConnection> backend;
	// remote failure closes recursively, and close takes a final snapshot
	// callbacks only acquire mutex, never operations
	std::recursive_mutex operations;
	mutable std::mutex mutex;
	::rtc::PeerConnection::State state = ::rtc::PeerConnection::State::New;
	std::optional<::rtc::PeerConnection::State> failure;
	::rtc::PeerConnection::GatheringState gathering = ::rtc::PeerConnection::GatheringState::New;
	std::optional<std::string> localDescription, fingerprint, localAddress, remoteAddress;
	std::vector<std::pair<std::string, std::string>> candidates;
	size_t remoteCandidates = 0, sent = 0, received = 0;
	std::optional<std::chrono::milliseconds> rtt;
	bool closed = false, admitted = false, candidateOverflow = false;
};

size_t active_peers();
size_t cleanup_failures();
void shutdown_request() noexcept;
bool shutdown_peers() noexcept;
void retain_library(const void* address);
void register_transport();
}
