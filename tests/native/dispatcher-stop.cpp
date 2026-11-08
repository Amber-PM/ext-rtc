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

// stopping the dispatcher must finish once the last backend call returned, also after a stall made it grow past two workers
// the idle extra workers used to park without ever seeing the stop request again
// every wait is bounded so a regression fails the test instead of hanging the run

#include "../../src/channel/channel.h"
#include "../../src/core/dispatcher.h"
#include <cstring>
#include <dlfcn.h>
#include <iostream>
#include <thread>

using namespace amber::rtc;
using namespace std::chrono_literals;
using clock_type = std::chrono::steady_clock;

namespace {
std::atomic<bool> releaseFirst{false}, releaseLast{false};
std::atomic<int> stuckFirst{0}, stuckLast{0};
}

// stalls in front of the backend, holding no backend lock, like a preempted or contended send call
bool stalled_send(::rtc::DataChannel* channel, const std::byte* data, size_t size, std::shared_ptr<void> owner)
	__asm__("_ZN3rtc11DataChannel4sendEPKSt4bytemSt10shared_ptrIvE");
bool stalled_send(::rtc::DataChannel* channel, const std::byte* data, size_t size, std::shared_ptr<void> owner){
	using Send = bool (*)(::rtc::DataChannel*, const std::byte*, size_t, std::shared_ptr<void>);
	static auto real = reinterpret_cast<Send>(dlsym(RTLD_NEXT, "_ZN3rtc11DataChannel4sendEPKSt4bytemSt10shared_ptrIvE"));
	if(size >= 8 && std::memcmp(data, "BLOCKME!", 8) == 0){
		++stuckFirst;
		while(!releaseFirst.load()){ releaseFirst.wait(false); }
	} else if(size >= 8 && std::memcmp(data, "BLOCKLST", 8) == 0){
		++stuckLast;
		while(!releaseLast.load()){ releaseLast.wait(false); }
	}
	return real(channel, data, size, std::move(owner));
}

namespace {
struct Pair{
	std::shared_ptr<Peer> a, b;
	std::shared_ptr<Channel> sender, receiver;
};

template <class F> bool within(std::chrono::milliseconds limit, F ready){
	auto end = clock_type::now() + limit;
	while(!ready()){
		if(clock_type::now() > end){ return false; }
		std::this_thread::sleep_for(1ms);
	}
	return true;
}

void check(bool value, const char* why){ if(!value){ throw std::runtime_error(why); } }

// the pinned backend loses a handshake now and then without ext-rtc being involved, the setup is built again, never the behaviour under test
Pair connect_once();
Pair connect(){
	for(int attempt = 0;; ++attempt){
		try{
			return connect_once();
		} catch(const std::runtime_error& error){
			if(attempt == 2){ throw; }
			std::cerr << "retrying the setup after: " << error.what() << std::endl;
		}
	}
}

Pair connect_once(){
	Pair pair;
	pair.a = Peer::create({});
	pair.b = Peer::create({});
	pair.sender = pair.a->create_channel("stop", {});
	check(within(10s, [&]{ pair.a->snapshot(); std::lock_guard lock(pair.a->mutex); return pair.a->gathering == ::rtc::PeerConnection::GatheringState::Complete; }), "gather a");
	pair.b->remote(*pair.a->localDescription, true);
	check(within(10s, [&]{ pair.b->snapshot(); std::lock_guard lock(pair.b->mutex); return pair.b->gathering == ::rtc::PeerConnection::GatheringState::Complete; }), "gather b");
	pair.a->remote(*pair.b->localDescription, false);
	check(within(10s, [&]{
		for(auto channel : pair.b->poll_channels()){ pair.receiver = channel; }
		return pair.receiver && pair.sender->is_open() && pair.receiver->is_open();
	}), "channel open");
	return pair;
}
}

int main(){
	try{
		auto first = connect(), second = connect(), last = connect();
		// two stuck peers make the dispatcher grow to three workers, then both calls return
		check(first.sender->send("BLOCKME!first"), "send refused");
		check(within(10s, []{ return stuckFirst.load() >= 1; }), "first peer never reached the backend");
		check(second.sender->send("BLOCKME!second"), "send refused");
		check(within(10s, []{ return stuckFirst.load() >= 2; }), "second peer never reached the backend");
		check(within(5s, []{ return dispatcher_workers() >= 3; }), "no replacement worker after the stall");
		releaseFirst = true;
		releaseFirst.notify_all();
		check(within(5s, []{ return dispatcher_stalled() == 0; }), "workers still counted as stuck");
		std::this_thread::sleep_for(300ms);
		// a peer is serviced inside the backend while it is queued again behind itself
		check(last.sender->send("BLOCKLST first"), "send refused");
		check(within(10s, []{ return stuckLast.load() >= 1; }), "last peer never reached the backend");
		for(int i = 0; i < 3; ++i){ check(last.sender->send("more"), "send refused"); std::this_thread::sleep_for(20ms); }
		std::atomic<bool> stopped{false};
		std::thread stopper([&]{ stop_dispatcher(); stopped = true; });
		std::this_thread::sleep_for(500ms);
		releaseLast = true;
		releaseLast.notify_all();
		const bool returned = within(5s, [&]{ return stopped.load(); });
		if(!returned){ std::cerr << "FAIL stop_dispatcher() did not return after the last backend call returned" << std::endl; std::_Exit(1); }
		stopper.join();
		std::cout << "PASS dispatcher-stop, stop_dispatcher() returned with " << dispatcher_workers() << " workers" << std::endl;
		return 0;
	} catch(const std::exception& error){
		releaseFirst = true;
		releaseLast = true;
		std::cerr << "FAIL " << error.what() << std::endl;
		std::_Exit(1);
	}
}
