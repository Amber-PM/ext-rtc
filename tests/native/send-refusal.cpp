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

// a send that only lacks global budget is refused with false so the caller retries, nothing is dropped or thrown

#include "../../src/channel/channel.h"
#include "../../src/core/dispatcher.h"
#include <iostream>
#include <thread>

using namespace amber::rtc;
using namespace std::chrono_literals;

namespace {
void check(bool value, const char* why){
	if(!value){ throw std::runtime_error(why); }
}

template<class F> void wait_for(F f, const char* why){
	const auto end = std::chrono::steady_clock::now() + 10s;
	while(!f()){
		if(std::chrono::steady_clock::now() > end){ throw std::runtime_error(why); }
		std::this_thread::sleep_for(2ms);
	}
}
}

int main(){
	try{
		const auto baseline = managed_buffer_bytes();
		{
			auto a = Peer::create({}), b = Peer::create({});
			auto channel = a->create_channel("refusal", {});
			wait_for([&]{ a->snapshot(); std::lock_guard lock(a->mutex); return a->gathering == ::rtc::PeerConnection::GatheringState::Complete; }, "gather a");
			b->remote(*a->localDescription, true);
			wait_for([&]{ b->snapshot(); std::lock_guard lock(b->mutex); return b->gathering == ::rtc::PeerConnection::GatheringState::Complete; }, "gather b");
			a->remote(*b->localDescription, false);
			std::shared_ptr<Channel> incoming;
			wait_for([&]{
				for(auto c : b->poll_channels()){ incoming = c; }
				return incoming && incoming->is_open() && channel->is_open();
			}, "channel open");
			const std::string payload(4096, 'x');
			// leave less budget than one message needs
			const size_t filler = bufferLimit - budget_bytes() - 1024;
			reserve_escrow(filler);
			const auto rejected = send_rejected_count();
			bool accepted = true;
			try{
				accepted = channel->send(payload);
			} catch(const Error& error){
				release_escrow(filler);
				throw std::runtime_error("send threw instead of refusing, code " + std::to_string(error.code));
			}
			check(!accepted, "send was accepted without budget");
			check(send_rejected_count() == rejected + 1, "refusal was not counted");
			check(send_footprint() == 0, "refused send kept a reservation");
			release_escrow(filler);
			// the same frame goes through once budget is back and arrives intact
			check(channel->send(payload), "retry was refused");
			std::string received;
			wait_for([&]{
				if(auto size = incoming->read_size()){
					received.resize(*size);
					return incoming->copy_front(received.data(), *size, true);
				}
				return false;
			}, "frame not delivered");
			check(received == payload, "frame changed in transit");
			a->close();
			b->close();
		}
		wait_for([&]{ return active_peers() == 0 && managed_buffer_bytes() == baseline && send_footprint() == 0; }, "budget did not drain");
		std::cout << "PASS a send without global budget is refused visibly and succeeds on retry\n";
	} catch(const std::exception& e){
		std::cerr << e.what() << '\n';
		return 1;
	}
}
