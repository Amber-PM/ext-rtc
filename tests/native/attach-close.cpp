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

// closing a peer or a channel while the backend delivers incoming channels must neither hang nor leak
// the close moment sweeps the whole negotiation, so some rounds close before the channels exist, some while they attach
// a watchdog turns a deadlock into a failure instead of a hung test run

#include "../../src/channel/channel.h"
#include "../../src/core/dispatcher.h"
#include <atomic>
#include <iostream>
#include <thread>

using namespace amber::rtc;
using namespace std::chrono_literals;
using clock_type = std::chrono::steady_clock;

namespace {
std::atomic<long> progress{0};

template <class F> bool within(std::chrono::milliseconds limit, F ready){
	auto end = clock_type::now() + limit;
	while(!ready()){
		if(clock_type::now() > end){ return false; }
		std::this_thread::sleep_for(1ms);
	}
	return true;
}

void check(bool value, const char* why){ if(!value){ throw std::runtime_error(why); } }

bool gathered(const std::shared_ptr<Peer>& peer){
	peer->snapshot();
	std::lock_guard lock(peer->mutex);
	return peer->gathering == ::rtc::PeerConnection::GatheringState::Complete;
}
}

int main(int argc, char** argv){
	const int rounds = argc > 1 ? std::stoi(argv[1]) : 120;
	std::thread([]{
		long last = -1;
		for(;;){
			std::this_thread::sleep_for(45s);
			if(progress.load() == last){
				std::cerr << "FAIL no progress for 45 s, deadlock" << std::endl;
				std::_Exit(2);
			}
			last = progress.load();
		}
	}).detach();
	try{
		size_t closedEarly = 0, closedMid = 0;
		for(int round = 0; round < rounds; ++round){
			auto a = Peer::create({}), b = Peer::create({});
			auto first = a->create_channel("first", {});
			auto second = a->create_channel("second", {});
			check(within(10s, [&]{ return gathered(a); }), "gather a");
			b->remote(*a->localDescription, true);
			check(within(10s, [&]{ return gathered(b); }), "gather b");
			a->remote(*b->localDescription, false);
			// 0 to 12 ms after the answer, the channels attach on a backend thread somewhere in this range
			std::this_thread::sleep_for(std::chrono::microseconds(round * 100));
			std::vector<std::shared_ptr<Channel>> incoming;
			std::thread poller([&]{
				auto end = clock_type::now() + 300ms;
				while(clock_type::now() < end){
					for(auto c : b->poll_channels()){ incoming.push_back(c); }
					std::this_thread::sleep_for(100us);
				}
			});
			// alternate what is closed: the receiving peer, the sending peer, or only one channel
			switch(round % 3){
			case 0: b->close(); break;
			case 1: a->close(); break;
			default: first->close_safely(); break;
			}
			poller.join();
			if(incoming.empty()){ ++closedEarly; } else{ ++closedMid; }
			a->close();
			b->close();
			incoming.clear();
			a = {}; b = {}; first = {}; second = {};
			check(within(15s, []{ return active_peers() == 0 && active_channels() == 0 && managed_buffer_bytes() == 0; }), "resources not released after the racing close");
			++progress;
		}
		stop_dispatcher();
		std::cout << "PASS attach-close, " << rounds << " rounds, closed before any channel arrived " << closedEarly << ", with channels delivered " << closedMid << std::endl;
		return 0;
	} catch(const std::exception& error){
		std::cerr << "FAIL " << error.what() << std::endl;
		return 1;
	}
}
