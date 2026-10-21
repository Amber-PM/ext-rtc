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
#include <cstring>
#include <iostream>
#include <thread>
using namespace amber::rtc;
using namespace std::chrono_literals;
void check(bool value,const char* why){if(!value){throw std::runtime_error(why);}}
template<class F> void wait(F ready, const std::string& why = "concurrent lifecycle deadline"){
	auto end=std::chrono::steady_clock::now()+30s;
	while(!ready()){
		if(std::chrono::steady_clock::now()>end){
			throw std::runtime_error(why);
		}
		std::this_thread::sleep_for(2ms);
	}
}
int main(){
	try{
		for(unsigned iteration=0;iteration<200;++iteration){
			Options options;options.pending=4;options.receiveMessages=1024;
			auto a=Peer::create(options),b=Peer::create(options);
			std::array<std::shared_ptr<Channel>,4> local,remote;
			for(unsigned i=0;i<4;++i){local[i]=a->create_channel(std::to_string(i),{});}
			wait([&]{a->snapshot();std::lock_guard lock(a->mutex);return a->gathering==::rtc::PeerConnection::GatheringState::Complete;},"gathering a");
			b->remote(*a->localDescription,true);
			wait([&]{b->snapshot();std::lock_guard lock(b->mutex);return b->gathering==::rtc::PeerConnection::GatheringState::Complete;},"gathering b");
			a->remote(*b->localDescription,false);
			size_t adopted=0;
			wait([&]{for(auto c:b->poll_channels()){remote[std::stoul(c->label)]=c;++adopted;}return adopted==4 && local[0]->is_open() && local[1]->is_open() && local[2]->is_open() && local[3]->is_open();},"channel open");
			std::array<std::thread,4> senders;
			std::atomic<bool> unexpected{false};
			for(unsigned i=0;i<4;++i){
				senders[i]=std::thread([&,i]{
					try{
						for(unsigned sequence=0;sequence<32;++sequence){
							std::string payload(8,'\0');std::memcpy(payload.data(),&i,4);std::memcpy(payload.data()+4,&sequence,4);
							if(!local[i]->send(payload)){unexpected=true;}
						}
					}catch(const Error& e){if(iteration%2==0 || (e.code!=2 && e.code!=3)){unexpected=true;}}
				});
			}
			if(iteration%2){
				std::thread closeA([a]{a->close();});
				std::thread closeAgain([a]{a->close();});
				closeA.join();closeAgain.join();
			}
			for(auto& sender:senders){sender.join();}
			check(!unexpected,"unexpected send result");
			if(iteration%2==0){
				wait([&]{std::lock_guard lock(b->queues->mutex);return b->queues->receiveMessages==128;},"receive 128 messages");
				for(unsigned i=0;i<4;++i){
					for(unsigned sequence=0;sequence<32;++sequence){
						char bytes[8];check(remote[i]->copy_front(bytes,8,true),"missing message");
						unsigned id,number;std::memcpy(&id,bytes,4);std::memcpy(&number,bytes+4,4);
						check(id==i && number==sequence,"duplicate, reorder or corrupt payload");
					}
					check(!remote[i]->read_size(),"extra message");
				}
			}
			a->close();b->close();a.reset();b.reset();
			for(auto& c:local){check(c->is_closed(),"retained local child revived");c->close();}
			for(auto& c:remote){check(c->is_closed(),"retained remote child revived");c->close();}
		}
		wait([]{return managed_buffer_bytes()==0 && active_channels()==0 && active_peers()==0;},"final cleanup");
		stop_dispatcher();
		std::cout<<"PASS 200 real SCTP cycles, four simultaneous channels, 12800 FIFO messages, concurrent send/close and retained handles\n";
	}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
