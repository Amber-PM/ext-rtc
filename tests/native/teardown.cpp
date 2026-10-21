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


#include "../../src/channel/channel.h"
#include "../../src/core/dispatcher.h"
#include <cerrno>
#include <cstdlib>
#include <dlfcn.h>
#include <future>
#include <iostream>
#include <source_location>
#include <sys/socket.h>
#include <thread>

using namespace amber::rtc;
using namespace std::chrono_literals;
struct socket;
std::atomic<struct socket*> blockedSocket{nullptr};
std::atomic<bool> blockSctp{false}, gateClose{false}, closeEntered{false}, permitClose{false};
std::atomic<size_t> socketCloses{0};
thread_local bool identifySocket = false;

namespace amber::rtc {
size_t budget_used() noexcept {
	size_t low = 0, high = bufferLimit;
	while(low < high){
		const auto amount = low + (high - low + 1) / 2;
		try{
			reserve_escrow(amount); release_escrow(amount); low = amount;
		} catch(const Error& error){
			if(error.code != 10){ std::abort(); }
			high = amount - 1;
		}
	}
	return bufferLimit - low;
}
}

extern "C" ssize_t usrsctp_sendv(struct socket* socket, const void* data, size_t size,
	struct sockaddr* to, int count, void* info, socklen_t length, unsigned int type, int flags){
	using Send = decltype(&usrsctp_sendv);
	static auto real = reinterpret_cast<Send>(dlsym(RTLD_NEXT, "usrsctp_sendv"));
	if(identifySocket){ blockedSocket = socket; identifySocket = false; }
	if(blockSctp && socket == blockedSocket){ errno = EWOULDBLOCK; return -1; }
	return real(socket, data, size, to, count, info, length, type, flags);
}

extern "C" void usrsctp_close(struct socket* socket){
	using Close = decltype(&usrsctp_close);
	static auto real = reinterpret_cast<Close>(dlsym(RTLD_NEXT, "usrsctp_close"));
	if(gateClose && socket == blockedSocket){
		closeEntered = true;
		closeEntered.notify_all();
		while(!permitClose.load()){ permitClose.wait(false); }
	}
	real(socket);
	++socketCloses;
}

void check(bool value, const char* why){ if(!value){ throw std::runtime_error(why); } }
template<class F> void spin(F ready, std::source_location where = std::source_location::current()){
	auto end = std::chrono::steady_clock::now() + 10s;
	while(!ready()){
		if(std::chrono::steady_clock::now() > end){ throw std::runtime_error("teardown deadline at " + std::to_string(where.line())); }
		std::this_thread::yield();
	}
}
template<class F> void released(F ready){
	spin(std::move(ready));
}

struct Pair{
	std::shared_ptr<Peer> a, b;
	std::array<std::shared_ptr<Channel>, 3> local, remote;
	Pair(){
		Options options; options.message = 64; options.sendBytes = 64; options.pending = 4;
		a = Peer::create(options); b = Peer::create(options);
		for(size_t i = 0; i < 3; ++i){ local[i] = a->create_channel(std::to_string(i), {}); }
		spin([&]{ a->snapshot(); std::lock_guard lock(a->mutex); return a->gathering == ::rtc::PeerConnection::GatheringState::Complete; });
		b->remote(*a->localDescription, true);
		spin([&]{ b->snapshot(); std::lock_guard lock(b->mutex); return b->gathering == ::rtc::PeerConnection::GatheringState::Complete; });
		a->remote(*b->localDescription, false);
		spin([&]{
			for(auto c : b->poll_channels()){ remote[std::stoul(c->label)] = c; }
			for(size_t i = 0; i < 3; ++i){ if(!remote[i] || !local[i]->is_open()){ return false; } }
			return true;
		});
	}
	~Pair(){ a->close_safely(); b->close_safely(); }
	void congest(){
		std::lock_guard operation(a->operations);
		blockSctp = true; identifySocket = true;
		check(local[0]->send(""), "empty admission rejected"); a->service();
		identifySocket = false;
		check(local[1]->send("pending-B"), "nonempty admission rejected"); a->service();
		std::lock_guard lock(a->queues->mutex);
		check(a->queues->submitted == 2 && a->queues->sendMessages == 2 && a->queues->waiting, "real SCTP debt not established");
	}
	void transfer(){
		std::string data("live\0peer", 9);
		check(local[2]->send(data), "other peer admission rejected");
		a->service();
		spin([&]{ return remote[2]->queued_messages() == 1; });
		char result[9];
		check(remote[2]->copy_front(result, 9, true) && std::string(result, 9) == data && !remote[2]->read_size(), "other peer corrupted or duplicated payload");
	}
};

void run(const std::string& mode){
	struct Release{
		~Release(){ permitClose = true; permitClose.notify_all(); blockSctp = false; }
	} release;
	if(mode == "normal"){
		{ Pair pair; pair.transfer(); pair.a->close(); pair.a->close(); }
		released([]{ return budget_used() == 0 && managed_buffer_bytes() == 0; });
		return;
	}
	if(mode == "construct-close"){
		for(size_t i = 0; i < 64; ++i){ auto peer = Peer::create({}); peer->create_channel("early", {}); peer->close(); }
		released([]{ return budget_used() == 0 && managed_buffer_bytes() == 0; });
		return;
	}
	auto pair = std::make_unique<Pair>();
	std::unique_ptr<Pair> other;
	if(mode == "peers"){ other = std::make_unique<Pair>(); }
	const auto baseline = budget_used();
	const auto owned = managed_buffer_bytes();
	pair->congest();
	std::weak_ptr<Escrow> first, second;
	size_t escrow;
	{
		std::lock_guard lock(pair->a->queues->mutex);
		auto& q = *pair->a->queues;
		first = q.sends[q.first].credit;
		second = q.sends[(q.first + 1) % q.sends.size()].credit;
		escrow = q.sends[q.first].escrow + q.sends[(q.first + 1) % q.sends.size()].escrow;
	}
	const auto debt = budget_used() - baseline;
	check(debt > escrow && managed_buffer_bytes() > owned, "escrow metadata uncharged");
	auto backend = pair->a->backend;
	if(mode == "concurrent"){
		std::atomic<bool> unexpected{false};
		std::thread sender([&]{
			try{ for(size_t i = 0; i < 10; ++i){ pair->local[2]->send("race"); } }
			catch(const Error& error){ if(error.code != 2 && error.code != 3){ unexpected = true; } }
		});
		std::array<std::thread, 8> closers;
		for(auto& closer : closers){ closer = std::thread([&]{ pair->a->close(); }); }
		for(auto& closer : closers){ closer.join(); }
		sender.join(); check(!unexpected, "unexpected concurrent send error");
	}else{
		pair->a->close();
	}
	check(budget_used() == baseline + debt, "peer close reused pending escrow budget");
	check(!first.expired() && !second.expired(), "peer close destroyed backend credit owners");
	check(backend->state() == ::rtc::PeerConnection::State::Connected && backend->bytesSent() == 0, "retained SCTP fixture drained unexpectedly");
	check(pair->a->queues->sendMessages == 0 && pair->a->queues->sendBytes == 0, "closed wrapper queue not purged");
	if(other){ other->transfer(); }
	gateClose = true;
	std::weak_ptr<::rtc::PeerConnection> publicWrapper = backend;
	backend.reset();
	spin([]{ return closeEntered.load(); });
	check(publicWrapper.expired(), "public wrapper unexpectedly retained");
	check(!first.expired() && !second.expired() && budget_used() == baseline + debt, "destructor entry freed backlog escrow");
	size_t filler = 0;
	if(mode == "budget"){
		filler = bufferLimit - budget_used(); reserve_escrow(filler);
		bool rejected = false;
		try{ reserve_escrow(1); } catch(const Error& error){ if(error.code != 10){ throw; } rejected = true; }
		check(rejected && budget_used() == bufferLimit, "closing peer allowed global budget oversubscription");
	}
	if(mode == "request"){
		shutdown_request();
		pair.reset();
		check(!first.expired() && !second.expired(), "request shutdown released asynchronous credit");
	}
	permitClose = true; permitClose.notify_all();
	if(mode != "request"){
		released([&]{ return budget_used() == baseline + filler && managed_buffer_bytes() == owned; });
		check(first.expired() && second.expired(), "completed backend retained credit owners");
	}
	if(filler){
		reserve_escrow(1); release_escrow(1);
		release_escrow(filler);
	}
	if(other){ other->transfer(); }
	other.reset(); pair.reset();
	released([]{ return budget_used() == 0 && managed_buffer_bytes() == 0 && active_peers() == 0 && active_channels() == 0; });
	check(first.expired() && second.expired() && socketCloses > 0, "backlog completion did not release resources");
	reserve_escrow(bufferLimit); release_escrow(bufferLimit);
	std::cout << "escrow=" << escrow << " metadata=" << debt - escrow << " exactly released after Message destruction\n";
}

int main(int argc, char** argv){
	try{
		run(argc == 2 ? argv[1] : "backlog");
		stop_dispatcher();
		std::cout << "PASS teardown ownership\n";
	}catch(const std::exception& error){ std::cerr << error.what() << '\n'; return 1; }
	return 0;
}
