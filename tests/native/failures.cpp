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
#include <iostream>
#include <thread>
using namespace amber::rtc;
using namespace std::chrono_literals;
void check(bool value,const char* why){if(!value){throw std::runtime_error(why);}}
template<class F> void wait(F ready){auto end=std::chrono::steady_clock::now()+3s;while(!ready()){if(std::chrono::steady_clock::now()>end){throw std::runtime_error("terminal receive work was not consumed");}std::this_thread::sleep_for(2ms);}}
int main(int argc,char** argv){
	try{
		const bool budget=argc==2 && std::string(argv[1])=="budget";
		auto a=Peer::create({}),b=Peer::create({});ChannelOptions options;
		options.reliability.unordered=true;options.reliability.maxRetransmits=0;
		auto channel=a->create_channel("native-failure",options);
		wait([&]{a->snapshot();std::lock_guard lock(a->mutex);return a->gathering==::rtc::PeerConnection::GatheringState::Complete;});
		b->remote(*a->localDescription,true);
		wait([&]{b->snapshot();std::lock_guard lock(b->mutex);return b->gathering==::rtc::PeerConnection::GatheringState::Complete;});
		a->remote(*b->localDescription,false);std::shared_ptr<Channel> incoming;
		wait([&]{auto c=b->poll_channels();if(!c.empty()){incoming=c[0];}return incoming && channel->is_open();});
		if(budget){
			std::string payload(bufferLimit-managed_buffer_bytes()-sizeof(Buffer),'x');
			auto* reservation=Buffer::copy(payload);
			channel->backend->send(reinterpret_cast<const std::byte*>("x"),1);
			wait([&]{return b->queues->overflows==1;});
			Buffer::release(reservation);
			bool explicitFailure=false;
			try{incoming->read_size();}catch(const Error& e){explicitFailure=e.code==10;}
			check(explicitFailure,"global unreliable RX failure was silently dropped");
		}else{channel->close();}
		wait([&]{std::lock_guard lock(incoming->lifecycle);return incoming->is_closed() && !incoming->backend && !incoming->closeRequested;});
		check(!b->dispatchStopped,"channel close unnecessarily closed peer");
		a->close();b->close();stop_dispatcher();
		std::cout<<"PASS explicit RX resource failure and terminal channel work consumption\n";
	}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
