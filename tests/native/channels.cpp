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
#include <chrono>
#include <iostream>
#include <thread>
using namespace std::chrono_literals;
using namespace amber::rtc;
void check(bool ok, const char* message){ if(!ok){ throw std::runtime_error(message); } }
template<class F> void wait(F f){
	auto end = std::chrono::steady_clock::now() + 10s;
	while(!f()){
		if(std::chrono::steady_clock::now() > end){ throw std::runtime_error("channel deadline"); }
		std::this_thread::sleep_for(2ms);
	}
}
int main(){
	try{
		validate_channel_text("generic\xc3\xb1");
		for(auto text : {std::string("a\0b",3), std::string("\xc0\x80",2), std::string(65536,'x')}){
			bool rejected = false;
			try{ validate_channel_text(text); }catch(const Error& e){ rejected = e.code == 1; }
			check(rejected,"invalid channel text accepted");
		}
		auto a = Peer::create({});
		auto b = Peer::create({});
		ChannelOptions o;
		o.reliability.unordered = true;
		o.reliability.maxRetransmits = 0;
		o.protocol = "binary";
		auto local = a->create_channel("arbitrary", o);
		std::shared_ptr<Channel> second;
		wait([&]{ a->snapshot(); return a->gathering == ::rtc::PeerConnection::GatheringState::Complete; });
		b->remote(*a->localDescription, true);
		wait([&]{ b->snapshot(); return b->gathering == ::rtc::PeerConnection::GatheringState::Complete; });
		a->remote(*b->localDescription, false);
		std::vector<std::shared_ptr<Channel>> incoming;
		wait([&]{ auto next = b->poll_channels(); incoming.insert(incoming.end(), next.begin(), next.end()); return incoming.size() == 1 && local->is_open(); });
		second = a->create_channel("ordered", {});
		wait([&]{ auto next = b->poll_channels(); incoming.insert(incoming.end(), next.begin(), next.end()); return incoming.size() == 2 && second->is_open(); });
		check(a->poll_channels().empty() && b->poll_channels().empty(),"duplicate channel handoff");
		check(incoming[0]->label == "arbitrary" && incoming[1]->label == "ordered","incoming FIFO");
		check(incoming[0]->settings.protocol == "binary" && incoming[0]->settings.reliability.unordered && incoming[0]->settings.reliability.maxRetransmits == 0,"incoming reliability");
		check(!incoming[1]->settings.reliability.maxRetransmits,"reliable metadata");
		check(active_channels() == 4,"active channel count");
		a->close(); b->close(); a.reset(); b.reset();
		check(local->is_closed() && !local->is_open() && local->label == "arbitrary", "retained child");
		local->close(); local->close();
		check(active_channels() == 0,"closed channel leak");
		a = Peer::create({});
		std::vector<std::shared_ptr<Channel>> channels;
		for(int i=0;i<16;++i){ channels.push_back(a->create_channel("bounded",{})); }
		bool rejected = false;
		try{ a->create_channel("excess",{}); }catch(const Error& e){ rejected = e.code == 8; }
		check(rejected,"channel 17 admitted");
		channels.front()->close();
		channels.push_back(a->create_channel("replacement",{}));
		a->close();
		check(active_channels() == 0,"live channel leak");
		std::cout << "PASS native channels, real SCTP, metadata, FIFO, limits and retained children\n";
	}catch(const std::exception& e){ std::cerr << e.what() << '\n'; return 1; }
}
