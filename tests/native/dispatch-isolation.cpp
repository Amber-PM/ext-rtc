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
#include <cstring>
#include <dlfcn.h>
#include <iostream>
#include <sys/socket.h>
#include <thread>

using namespace amber::rtc;
using namespace std::chrono_literals;

namespace {
std::atomic<bool> armed{false}, entered{false}, release{false};
}

struct socket;
extern "C" ssize_t usrsctp_sendv(struct socket* socket, const void* data, size_t length, struct sockaddr* to,
	int count, void* info, socklen_t infoLength, unsigned int type, int flags){
	using Send = decltype(&usrsctp_sendv);
	static auto real = reinterpret_cast<Send>(dlsym(RTLD_NEXT, "usrsctp_sendv"));
	if(armed && length >= 8 && std::memcmp(data, "BLOCKME!", 8) == 0){
		entered = true;
		entered.notify_all();
		while(!release.load()){ release.wait(false); }
	}
	return real(socket, data, length, to, count, info, infoLength, type, flags);
}

namespace {
struct Pair{
	std::shared_ptr<Peer> a, b;
	std::shared_ptr<Channel> sender, receiver;
};

void check(bool value, const char* why){ if(!value){ throw std::runtime_error(why); } }

template <class F> bool within(std::chrono::milliseconds limit, F ready){
	auto end = std::chrono::steady_clock::now() + limit;
	while(!ready()){
		if(std::chrono::steady_clock::now() > end){ return false; }
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
	pair.sender = pair.a->create_channel("isolation", {});
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
}

int main(){
	try{
		auto stuck = connect(), other = connect();
		armed = true;
		check(stuck.sender->send("BLOCKME!first"), "blocking send refused");
		check(stuck.sender->send("second"), "queued send refused");
		check(within(10s, []{ return entered.load(); }), "dispatcher never reached the blocked send");
		// one worker is parked inside the backend for the first peer, other peers must still be served
		check(other.sender->send("independent"), "independent send refused");
		const bool served = within(3s, [&]{ return other.receiver->queued_messages() == 1; });
		release = true;
		release.notify_all();
		check(served, "a stalled peer blocked the dispatcher for every other peer");
		check(read(other.receiver) == "independent", "independent payload corrupt");
		check(within(10s, [&]{ return stuck.receiver->queued_messages() == 2; }), "stalled peer never resumed");
		check(read(stuck.receiver) == "BLOCKME!first" && read(stuck.receiver) == "second", "per peer order broken");
		armed = false;
		stuck.a->close(); stuck.b->close(); other.a->close(); other.b->close();
		stuck = {}; other = {};
		check(within(10s, []{ return active_peers() == 0 && active_channels() == 0 && managed_buffer_bytes() == 0; }), "resources not released");
		stop_dispatcher();
		std::cout << "PASS a peer stuck inside the backend does not block other peers and keeps its own order\n";
		return 0;
	} catch(const std::exception& error){
		release = true;
		release.notify_all();
		std::cerr << error.what() << '\n';
		std::_Exit(1);
	}
}
