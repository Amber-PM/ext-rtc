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

// a native send stuck inside the backend must not hold the caller thread of the same peer hostage
// every scenario bounds its waits so a regression fails instead of hanging the test run

#include "../../src/channel/channel.h"
#include "../../src/core/dispatcher.h"
#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace amber::rtc;
using namespace std::chrono_literals;
using clock_type = std::chrono::steady_clock;

namespace {
std::atomic<bool> armed{false}, entered{false}, release{false};
std::atomic<int> blockedCalls{0};
std::atomic<bool> throwOnRelease{false};
}

// stalls in front of the backend, holding no backend lock, like a preempted or contended send call
bool stalled_send(::rtc::DataChannel* channel, const std::byte* data, size_t size, std::shared_ptr<void> owner)
	__asm__("_ZN3rtc11DataChannel4sendEPKSt4bytemSt10shared_ptrIvE");
bool stalled_send(::rtc::DataChannel* channel, const std::byte* data, size_t size, std::shared_ptr<void> owner){
	using Send = bool (*)(::rtc::DataChannel*, const std::byte*, size_t, std::shared_ptr<void>);
	static auto real = reinterpret_cast<Send>(dlsym(RTLD_NEXT, "_ZN3rtc11DataChannel4sendEPKSt4bytemSt10shared_ptrIvE"));
	if(armed && size >= 8 && std::memcmp(data, "BLOCKME!", 8) == 0){
		++blockedCalls;
		entered = true;
		entered.notify_all();
		while(!release.load()){ release.wait(false); }
		// what usrsctp reports when the association is torn down under a send
		if(throwOnRelease.load()){ throw std::runtime_error("Sending failed, errno=32"); }
	}
	return real(channel, data, size, std::move(owner));
}

namespace {
struct Pair{
	std::shared_ptr<Peer> a, b;
	std::shared_ptr<Channel> sender, receiver;
};

void check(bool value, const char* why){ if(!value){ throw std::runtime_error(why); } }

template <class F> bool within(std::chrono::milliseconds limit, F ready){
	auto end = clock_type::now() + limit;
	while(!ready()){
		if(clock_type::now() > end){ return false; }
		std::this_thread::sleep_for(1ms);
	}
	return true;
}

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
	pair.sender = pair.a->create_channel("blocked", {});
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

std::optional<std::string> read(const std::shared_ptr<Channel>& channel){
	auto size = channel->read_size();
	if(!size){ return {}; }
	std::string result(*size, '\0');
	return channel->copy_front(result.data(), *size, true) ? std::optional(result) : std::nullopt;
}

// runs an operation on its own thread so the test can bound the wait
struct Call{
	std::shared_ptr<std::atomic<int>> state = std::make_shared<std::atomic<int>>(0);
	std::shared_ptr<std::atomic<long>> micros = std::make_shared<std::atomic<long>>(-1);
	std::thread thread;
	template <class F> explicit Call(F operation){
		auto started = clock_type::now();
		thread = std::thread([state = state, micros = micros, operation = std::move(operation), started]() mutable {
			int result = 1;
			try{ result = operation() ? 1 : 2; } catch(...){ result = 3; }
			micros->store(std::chrono::duration_cast<std::chrono::microseconds>(clock_type::now() - started).count());
			state->store(result);
		});
	}
	bool finished(std::chrono::milliseconds limit){ return within(limit, [&]{ return state->load() != 0; }); }
	~Call(){ if(thread.joinable()){ thread.join(); } }
};

void unblock(){
	release = true;
	release.notify_all();
}

struct Scenario{
	Pair stuck, other;
	std::vector<Pair> more;
	// extra stuck peers keep more dispatcher workers inside the backend at the same time
	explicit Scenario(int blocked = 1){
		release = false;
		entered = false;
		blockedCalls = 0;
		armed = false;
		stuck = connect();
		for(int i = 1; i < blocked; ++i){ more.push_back(connect()); }
		other = connect();
		armed = true;
		check(stuck.sender->send("BLOCKME!first"), "blocking send refused");
		for(auto& pair : more){ check(pair.sender->send("BLOCKME!more"), "extra blocking send refused"); }
		check(within(10s, [&]{ return blockedCalls.load() >= blocked; }), "dispatcher never reached the blocked sends");
	}
	void settle(){
		unblock();
		armed = false;
		stuck.a->close(); stuck.b->close(); other.a->close(); other.b->close();
		for(auto& pair : more){ pair.a->close(); pair.b->close(); }
		more.clear();
		stuck = {}; other = {};
		check(within(10s, []{ return active_peers() == 0 && active_channels() == 0 && managed_buffer_bytes() == 0; }), "resources not released");
	}
};

// close racing a sender that keeps the dispatcher busy, no stall injected
int race(){
	for(int round = 0; round < 25; ++round){
		armed = false;
		auto pair = connect();
		std::atomic<bool> stop{false};
		std::atomic<long> sent{0};
		std::thread sender([&]{
			const std::string payload(1024, 'x');
			while(!stop){
				try{ if(pair.sender->send(payload)){ ++sent; } else{ std::this_thread::yield(); } } catch(const Error&){ return; }
			}
		});
		const bool progressed = within(2s, [&]{ return sent.load() > 20 + round; });
		std::this_thread::sleep_for(std::chrono::microseconds(round * 300));
		pair.a->close();
		stop = true;
		sender.join();
		check(progressed, "the sender made no progress before the close");
		pair.b->close();
		pair = {};
		check(within(10s, []{ return active_peers() == 0 && active_channels() == 0 && managed_buffer_bytes() == 0; }), "resources not released after racing close");
	}
	stop_dispatcher();
	return 0;
}

// blocked peers per mode: permanent ones stay stuck until the test ends, recoverable ones are released after a pause
int blocked_peers(const std::string& mode){
	if(mode == "two"){ return 2; }
	if(mode == "six"){ return 6; }
	if(mode == "eight"){ return 8; }
	if(mode == "sixteen"){ return 16; }
	if(mode == "full"){ return 32; }
	if(mode == "beyond"){ return 48; }
	if(mode == "recoverable"){ return 12; }
	return 1;
}

// a peer closed while its backend call is stuck leaves the queue purge to the worker, a call that then throws must not skip it
int throwclose(){
	release = false;
	entered = false;
	blockedCalls = 0;
	armed = true;
	throwOnRelease = true;
	auto pair = connect();
	check(pair.sender->send("BLOCKME!first"), "blocking send refused");
	for(int i = 0; i < 4; ++i){ check(pair.sender->send(std::string(1000, 'x')), "queued send refused"); }
	check(within(10s, []{ return entered.load(); }), "dispatcher never reached the blocked send");
	pair.a->close();
	pair.b->close();
	unblock();
	pair = {};
	const bool freed = within(10s, []{ return active_peers() == 0 && active_channels() == 0 && managed_buffer_bytes() == 0 && send_footprint() == 0; });
	std::cout << "after the throwing call: footprint " << send_footprint() << " managed " << managed_buffer_bytes() << "\n";
	armed = false;
	throwOnRelease = false;
	check(freed, "queued sends of the closed peer were never released");
	stop_dispatcher();
	return 0;
}

// peers closed while their backend call stays stuck keep a worker each, a live stuck peer must not leave the healthy one without
int orphans(){
	release = false;
	entered = false;
	blockedCalls = 0;
	armed = true;
	constexpr int doomedCount = 6;
	std::vector<Pair> doomed;
	for(int i = 0; i < doomedCount; ++i){ doomed.push_back(connect()); }
	for(int i = 0; i < doomedCount; ++i){
		check(doomed[i].sender->send("BLOCKME!doomed"), "blocking send refused");
		check(within(10s, [&]{ return blockedCalls.load() >= i + 1; }), "the blocked call was not reached");
		// the supervisor flags the worker and starts its replacement
		std::this_thread::sleep_for(120ms);
	}
	for(auto& pair : doomed){ pair.a->close(); pair.b->close(); }
	doomed.clear();
	check(within(5s, []{ return active_peers() == 0; }), "peers not closed");
	auto stuck = connect();
	auto healthy = connect();
	check(stuck.sender->send("BLOCKME!live"), "blocking send refused");
	check(within(10s, [&]{ return blockedCalls.load() >= doomedCount + 1; }), "the live blocked call was not reached");
	std::this_thread::sleep_for(300ms);
	check(healthy.sender->send("hello"), "healthy send refused");
	const bool delivered = within(3s, [&]{ return healthy.receiver->queued_messages() == 1; });
	std::cout << "closed peers with stuck calls " << doomedCount << ", workers " << dispatcher_workers() << ", stalled " << dispatcher_stalled() << ", healthy peer " << (delivered ? "served" : "starved") << "\n";
	unblock();
	armed = false;
	check(delivered, "closed peers with stuck calls left the healthy peer without a worker");
	stuck.a->close(); stuck.b->close(); healthy.a->close(); healthy.b->close();
	stop_dispatcher();
	return 0;
}

int run(const std::string& mode){
	if(mode == "race"){ return race(); }
	if(mode == "throwclose"){ return throwclose(); }
	if(mode == "orphans"){ return orphans(); }
	const int blocked = blocked_peers(mode);
	Scenario s(blocked);
	// the second peer must keep working whatever happens to the first
	const auto started = clock_type::now();
	check(s.other.sender->send("independent"), "independent send refused");
	check(within(3s, [&]{ return s.other.receiver->queued_messages() == 1; }), "independent peer starved");
	check(read(s.other.receiver) == "independent", "independent payload corrupt");
	if(blocked > 1){
		const auto healthyMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - started).count();
		const size_t workers = dispatcher_workers();
		std::cout << blockedCalls.load() << " backend calls stuck, healthy peer delivered in " << healthyMs << " ms, "
			<< dispatcher_stalled() << " stalled of " << workers << " workers\n";
		// the thread count never passes two plus the connected peers, however many calls are stuck
		check(workers <= 2 + active_peers(), "dispatcher grew past its thread cap");
		// one replacement per stuck call and the base pair, a thread that no stall asked for is a bug
		check(workers <= static_cast<size_t>(blocked) + 2, "dispatcher started threads that no stall asked for");
		if(mode == "recoverable"){
			// a stall that ends must give every worker back
			std::this_thread::sleep_for(300ms);
			unblock();
			check(within(10s, []{ return dispatcher_stalled() == 0; }), "workers stayed counted as stuck after the backend calls returned");
		} else{
			check(dispatcher_stalled() == static_cast<size_t>(blocked), "stuck workers were not all counted as stalled");
			unblock();
		}
		auto delivered = [&](Pair& pair, const char* text){
			check(within(10s, [&]{ return pair.receiver->queued_messages() == 1; }), "a stuck peer never resumed");
			check(read(pair.receiver) == text, "stuck peer payload corrupt");
		};
		delivered(s.stuck, "BLOCKME!first");
		for(auto& pair : s.more){ delivered(pair, "BLOCKME!more"); }
		// the replacement workers leave once the stall is over, a pool that only grows keeps threads for the life of the process
		check(within(20s, []{ return dispatcher_workers() == 2; }), "replacement workers were not retired after the stall");
		// the dispatcher must be back to normal service
		check(s.other.sender->send("after"), "healthy send refused after the backend calls returned");
		check(within(3s, [&]{ return s.other.receiver->queued_messages() == 1; }), "healthy peer starved after the backend calls returned");
		check(read(s.other.receiver) == "after", "healthy payload corrupt after the backend calls returned");
	} else if(mode == "send"){
		// the game thread keeps queueing frames for a peer whose backend call is stuck
		Call call([&]{
			for(int i = 0; i < 5; ++i){ if(!s.stuck.sender->send("after" + std::to_string(i))){ return false; } }
			return true;
		});
		const bool returned = call.finished(2s);
		const long micros = call.micros->load();
		unblock();
		check(returned, "send() waited for the stuck backend call");
		check(call.state->load() == 1, "send() was refused although the queue had room");
		std::cout << "send() returned in " << micros << " us while the backend call was stuck\n";
		check(within(10s, [&]{ return s.stuck.receiver->queued_messages() == 6; }), "stuck peer never resumed");
		check(read(s.stuck.receiver) == "BLOCKME!first", "per peer order broken after unblocking");
		for(int i = 0; i < 5; ++i){ check(read(s.stuck.receiver) == "after" + std::to_string(i), "per peer order broken after unblocking"); }
	} else if(mode == "close"){
		Call call([&]{ s.stuck.a->close(); return true; });
		const bool returned = call.finished(3s);
		const long micros = call.micros->load();
		unblock();
		check(returned, "close() waited for the stuck backend call");
		std::cout << "close() returned in " << micros << " us while the backend call was stuck\n";
		{ std::lock_guard lock(s.stuck.a->mutex); check(s.stuck.a->closed, "peer still open after close()"); }
	} else if(mode == "snapshot"){
		Call call([&]{ s.stuck.a->snapshot(); return true; });
		const bool returned = call.finished(2s);
		const long micros = call.micros->load();
		unblock();
		check(returned, "snapshot() waited for the stuck backend call");
		std::cout << "snapshot() returned in " << micros << " us while the backend call was stuck\n";
	} else if(mode == "shutdown"){
		// shutdown cannot abandon a worker inside the backend, it must finish once the call returns
		Call call([&]{ stop_dispatcher(); return true; });
		const bool early = call.finished(300ms);
		unblock();
		check(call.finished(10s), "stop_dispatcher() never returned after the backend call returned");
		std::cout << "stop_dispatcher() " << (early ? "returned while stuck" : "waited for the stuck call") << ", " << call.micros->load() << " us\n";
		// the peers have to be released even though the dispatcher is gone
		s.settle();
		return 0;
	} else{
		throw std::runtime_error("unknown mode");
	}
	s.settle();
	stop_dispatcher();
	return 0;
}
}

int main(int argc, char** argv){
	try{
		const int status = run(argc > 1 ? argv[1] : "send");
		std::cout << "PASS blocked-send " << (argc > 1 ? argv[1] : "send") << '\n';
		return status;
	} catch(const std::exception& error){
		unblock();
		std::cerr << "FAIL " << error.what() << '\n';
		std::_Exit(1);
	}
}
