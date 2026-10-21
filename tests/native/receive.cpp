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
#include <iostream>
#include <thread>
using namespace amber::rtc;
using namespace std::chrono_literals;
void check(bool value, const char* why){ if(!value){ throw std::runtime_error(why); } }
template<class F> void wait(F f){ auto end=std::chrono::steady_clock::now()+10s; while(!f()){ if(std::chrono::steady_clock::now()>end){ throw std::runtime_error("receive deadline"); } std::this_thread::sleep_for(2ms); } }
std::optional<std::string> read(const std::shared_ptr<Channel>& c, bool consume){
	auto size=c->read_size(); if(!size){ return {}; }
	std::string value(*size,'\0');
	return c->copy_front(value.data(),value.size(),consume) ? std::optional(value) : std::nullopt;
}
int main(){
	try{
		for(bool unreliable : {false,true}){
			Options o; o.message=4; o.receiveBytes=4; o.receiveMessages=2;
			auto a=Peer::create(o), b=Peer::create(o);
			ChannelOptions setting; if(unreliable){ setting.reliability.unordered=true; setting.reliability.maxRetransmits=0; }
			auto local=a->create_channel("receive",setting);
			wait([&]{ a->snapshot(); return a->gathering==::rtc::PeerConnection::GatheringState::Complete; });
			b->remote(*a->localDescription,true);
			wait([&]{ b->snapshot(); return b->gathering==::rtc::PeerConnection::GatheringState::Complete; });
			a->remote(*b->localDescription,false);
			std::shared_ptr<Channel> incoming;
			wait([&]{ auto channels=b->poll_channels(); if(!channels.empty()){ incoming=channels[0]; } return incoming && local->is_open(); });
			check(!read(incoming,false),"empty queue");
			std::string binary("a\0\xffz",4);
			local->backend->send(reinterpret_cast<const std::byte*>(binary.data()),binary.size());
			wait([&]{ return incoming->queued_messages()==1; });
			check(incoming->available_bytes()==4,"payload bytes");
			check(read(incoming,false)==binary && read(incoming,false)==binary,"peek consumes or corrupts");
			check(read(incoming,true)==binary && !read(incoming,true),"receive once");
			local->backend->send(reinterpret_cast<const std::byte*>(""),0);
			wait([&]{ return incoming->queued_messages()==1; });
			check(read(incoming,false)=="" && read(incoming,true)=="" && !read(incoming,true),"empty payload distinction");
			local->backend->send(reinterpret_cast<const std::byte*>(binary.data()),4);
			wait([&]{ return incoming->queued_messages()==1; });
			local->backend->send(reinterpret_cast<const std::byte*>("x"),1);
			wait([&]{ return b->queues->overflows==1; });
			if(unreliable){
				check(read(incoming,true)==binary,"unreliable head evicted");
				check(incoming->is_open(),"unreliable overflow closes");
			}else{
				bool failed=false;
				try{ read(incoming,true); }catch(const Error& e){ failed=e.code==9; }
				check(failed,"reliable partial stream exposed");
				wait([&]{ return incoming->is_closed(); });
				incoming->close();
				failed=false;
				try{ read(incoming,false); }catch(const Error& e){ failed=e.code==9; }
				check(failed,"overflow latch lost on close");
			}
			a->close(); b->close();
			check(b->queues->receiveBytes==0 && b->queues->receiveMessages==0,"RX budget leak");
		}
		wait([]{ return managed_buffer_bytes()==0; });
		std::cout << "PASS binary/NUL/empty RX, exact limits, peek/receive, reliable latch and unreliable drop-new\n";
	}catch(const std::exception& e){ std::cerr<<e.what()<<'\n'; return 1; }
}
