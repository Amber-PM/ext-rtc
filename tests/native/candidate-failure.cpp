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

#include "../../src/peer.h"
#include <rtc/global.hpp>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

namespace {
std::atomic<bool> armed{false}, entered{false}, resumeCallback{false};
std::thread::id mainThread;

template <class F> void wait(F ready){
	auto deadline = std::chrono::steady_clock::now() + 10s;
	while(!ready()){
		if(std::chrono::steady_clock::now() > deadline){
			std::_Exit(2);
		}
		std::this_thread::sleep_for(1ms);
	}
}
}

// wrap only calls from this executable, leaving backend calls unchanged
// this linker hook uses the linux libstdc++ abi of the locked test targets
extern "C" std::string __real__ZNK3rtc9Candidate9candidateB5cxx11Ev(const ::rtc::Candidate*);

extern "C" std::string __wrap__ZNK3rtc9Candidate9candidateB5cxx11Ev(const ::rtc::Candidate* candidate){
	if(std::this_thread::get_id() != mainThread && armed.exchange(false)){
		entered = true;
		wait([] { return resumeCallback.load(); });
		throw std::bad_alloc();
	}
	return __real__ZNK3rtc9Candidate9candidateB5cxx11Ev(candidate);
}

int main(){
	try{
		mainThread = std::this_thread::get_id();
		::rtc::Configuration config;
		config.disableAutoNegotiation = true;
		auto offerer = std::make_shared<::rtc::PeerConnection>(config);
		auto channel = offerer->createDataChannel("candidate-failure");
		offerer->setLocalDescription(::rtc::Description::Type::Offer);
		wait([&] { return offerer->gatheringState() == ::rtc::PeerConnection::GatheringState::Complete; });
		auto peer = amber::rtc::Peer::create({});
		armed = true;
		peer->remote(std::string(*offerer->localDescription()), true);
		wait([] { return entered.load(); });
		std::thread closer([&] { peer->close(); });
		wait([&] {
			std::lock_guard lock(peer->mutex);
			return peer->closed;
		});
		resumeCallback = true;
		closer.join();
		bool closed;
		{
			std::lock_guard lock(peer->mutex);
			closed = peer->state == ::rtc::PeerConnection::State::Closed && !peer->failure;
		}
		peer.reset();
		offerer->close();
		channel.reset();
		offerer.reset();
		amber::rtc::shutdown_request();
		auto done = ::rtc::Cleanup();
		if(done.wait_for(10s) != std::future_status::ready){
			std::_Exit(4);
		}
		done.get();
		if(!closed){
			std::cerr << "late candidate allocation failure overwrote CLOSED\n";
			return 1;
		}
		std::cout << "PASS late candidate allocation failure preserves CLOSED\n";
		return 0;
	} catch(...){
		return 5;
	}
}
