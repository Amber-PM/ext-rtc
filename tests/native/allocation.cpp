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
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {
thread_local bool failNextAllocation = false;
}

void* operator new(std::size_t size){
	if(failNextAllocation){
		failNextAllocation = false;
		throw std::bad_alloc();
	}
	if(auto* memory = std::malloc(size ? size : 1)){
		return memory;
	}
	throw std::bad_alloc();
}

void operator delete(void* memory) noexcept {
	std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
	std::free(memory);
}

int main(){
	using namespace std::chrono_literals;
	try{
		::rtc::Configuration config;
		config.disableAutoNegotiation = true;
		auto offerer = std::make_shared<::rtc::PeerConnection>(config);
		auto channel = offerer->createDataChannel("allocation");
		offerer->setLocalDescription(::rtc::Description::Type::Offer);
		auto deadline = std::chrono::steady_clock::now() + 10s;
		while(offerer->gatheringState() != ::rtc::PeerConnection::GatheringState::Complete){
			if(std::chrono::steady_clock::now() > deadline){
				return 2;
			}
			std::this_thread::sleep_for(5ms);
		}
		auto peer = amber::rtc::Peer::create({});
		peer->remote(std::string(*offerer->localDescription()), true);
		deadline = std::chrono::steady_clock::now() + 10s;
		while(peer->backend->gatheringState() != ::rtc::PeerConnection::GatheringState::Complete){
			if(std::chrono::steady_clock::now() > deadline){
				return 2;
			}
			std::this_thread::sleep_for(5ms);
		}
		offerer->setRemoteDescription(
			::rtc::Description(std::string(*peer->backend->localDescription()), ::rtc::Description::Type::Answer));
		while(true){
			{
				std::lock_guard lock(peer->mutex);
				if(peer->state == ::rtc::PeerConnection::State::Connected){
					break;
				}
			}
			if(std::chrono::steady_clock::now() > deadline){
				return 2;
			}
			std::this_thread::sleep_for(5ms);
		}
		// the first fingerprint copy allocates here even when the backend is a dll
		if(peer->fingerprint){
			return 5;
		}
		bool diagnosed = false;
		failNextAllocation = true;
		try{
			peer->close();
		} catch(const amber::rtc::Error& e){
			diagnosed = e.code == 11;
		}
		if(!diagnosed || !peer->closed || peer->backend || amber::rtc::active_peers() != 0){
			std::cerr << "allocation failure bypassed cleanup\n";
			return 3;
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
		std::cout << "PASS snapshot allocation failure still closes peer\n";
		return 0;
	} catch(...){
		std::cerr << "allocation test failed\n";
		return 1;
	}
}
