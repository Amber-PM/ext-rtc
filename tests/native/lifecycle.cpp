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

#include "../../src/peer/peer.h"
#include <rtc/global.hpp>
#include <chrono>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

namespace {
template <class F> void wait(F ready){
	auto deadline = std::chrono::steady_clock::now() + 10s;
	while(!ready()){
		if(std::chrono::steady_clock::now() > deadline){
			throw std::runtime_error("lifecycle deadline");
		}
		std::this_thread::sleep_for(5ms);
	}
}
}

int main(){
	try{
		for(int i = 0; i < 24; ++i){
			::rtc::Configuration config;
			config.disableAutoNegotiation = true;
			auto offerer = std::make_shared<::rtc::PeerConnection>(config);
			auto channel = offerer->createDataChannel("lifecycle");
			offerer->setLocalDescription(::rtc::Description::Type::Offer);
			wait([&] { return offerer->gatheringState() == ::rtc::PeerConnection::GatheringState::Complete; });
			auto peer = amber::rtc::Peer::create({});
			peer->remote(std::string(*offerer->localDescription()), true);
			if(i % 3 != 0){
				wait([&] {
					std::lock_guard lock(peer->mutex);
					return peer->gathering == ::rtc::PeerConnection::GatheringState::Complete;
				});
				peer->snapshot();
				offerer->setRemoteDescription(
					::rtc::Description(*peer->localDescription, ::rtc::Description::Type::Answer));
				wait([&] {
					std::lock_guard lock(peer->mutex);
					return peer->state == ::rtc::PeerConnection::State::Connected;
				});
			}
			std::thread first([peer] { peer->close(); });
			std::thread second([peer] {
				peer->snapshot();
				peer->close();
			});
			first.join();
			second.join();
			if(i % 2){
				offerer->close();
				peer.reset();
			} else{
				peer.reset();
				offerer->close();
			}
			channel.reset();
			offerer.reset();
		}
		amber::rtc::shutdown_request();
		auto foreignPeer = std::make_shared<::rtc::PeerConnection>();
		if(amber::rtc::active_peers() != 0 || !amber::rtc::shutdown_peers()){
			throw std::runtime_error("cleanup failed");
		}
		foreignPeer->close();
		foreignPeer.reset();
		auto done = ::rtc::Cleanup();
		if(done.wait_for(10s) != std::future_status::ready){
			std::_Exit(2);
		}
		done.get();
		std::cout << "PASS 24 lifecycle iterations, 16 real handshakes, concurrent close and snapshots\n";
		return 0;
	} catch(const std::exception& e){
		std::cerr << e.what() << '\n';
		return 1;
	}
}
