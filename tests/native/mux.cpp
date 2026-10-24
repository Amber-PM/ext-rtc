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
template<class F> void wait(F ready, const char* why){
	auto end=std::chrono::steady_clock::now()+30s;
	while(!ready()){
		if(std::chrono::steady_clock::now()>end){throw std::runtime_error(why);}
		std::this_thread::sleep_for(2ms);
	}
}
void gathered(const std::shared_ptr<Peer>& peer){
	wait([&]{peer->snapshot();std::lock_guard lock(peer->mutex);return peer->gathering==::rtc::PeerConnection::GatheringState::Complete;},"gathering");
	peer->snapshot();
}
std::string receive(const std::shared_ptr<Channel>& channel){
	std::optional<size_t> size;
	wait([&]{return (size=channel->read_size()).has_value();},"receive");
	std::string payload(*size,'\0');
	check(channel->copy_front(payload.data(),payload.size(),true),"read payload");
	return payload;
}
Options plain(){
	Options options;
	options.config.bindAddress="127.0.0.1";
	return options;
}
Options mux(std::optional<std::string> bind,uint16_t port){
	Options options;
	options.config.bindAddress=std::move(bind);
	options.config.portRangeBegin=options.config.portRangeEnd=port;
	options.config.enableIceUdpMux=true;
	return options;
}
int main(){
	try{
		uint16_t port=0;
		for(uint16_t candidate=31000;candidate<31200 && !port;++candidate){
			try{Peer::create(mux("127.0.0.1",candidate))->close();port=candidate;}
			catch(const Error& e){if(e.code!=1){throw;}}
		}
		check(port!=0,"no free mux port");
		const std::string host=" 127.0.0.1 "+std::to_string(port)+" typ host";
		auto offer=Peer::create(plain()),answer=Peer::create(mux("127.0.0.1",port));
		auto local=offer->create_channel("live",{});
		gathered(offer);
		answer->remote(*offer->localDescription,true);
		gathered(answer);
		check(answer->localDescription->find(host)!=std::string::npos,"shared mux candidate");
		offer->remote(*answer->localDescription,false);
		std::shared_ptr<Channel> remote;
		wait([&]{for(auto c:answer->poll_channels()){remote=c;}return remote && remote->is_open() && local->is_open();},"channel open");
		answer->snapshot();
		check(answer->localAddress=="127.0.0.1:"+std::to_string(port),"selected pair uses the shared socket");
		// other agents join and leave the shared socket while the live pair keeps exchanging data
		std::atomic<bool> failed{false};
		std::atomic<unsigned> churned{0};
		std::array<std::thread,4> workers;
		for(unsigned worker=0;worker<workers.size();++worker){
			workers[worker]=std::thread([&,worker]{
				try{
					for(unsigned iteration=0;iteration<25;++iteration){
						auto peerOffer=Peer::create(plain()),peerAnswer=Peer::create(mux("127.0.0.1",port));
						auto channel=peerOffer->create_channel(std::to_string(worker),{});
						gathered(peerOffer);
						peerAnswer->remote(*peerOffer->localDescription,true);
						gathered(peerAnswer);
						check(peerAnswer->localDescription->find(host)!=std::string::npos,"churn candidate shares the socket");
						if(iteration%2){std::thread([peerAnswer]{peerAnswer->close();}).join();peerOffer->close();}
						else{peerOffer->close();peerAnswer->close();}
						channel->close();
						++churned;
					}
				}catch(const std::exception& e){std::cerr<<"worker "<<worker<<": "<<e.what()<<'\n';failed=true;}
			});
		}
		unsigned exchanged=0;
		while(churned<100 && !failed){
			const std::string id=std::to_string(exchanged);
			check(local->send("offer "+id) && remote->send("answer "+id),"live send");
			check(receive(remote)=="offer "+id && receive(local)=="answer "+id,"live payloads stay isolated and ordered");
			++exchanged;
		}
		for(auto& worker:workers){worker.join();}
		check(!failed,"concurrent mux churn failed");
		check(churned==100 && exchanged>0,"churn and live exchange both ran");
		check(local->send("after") && receive(remote)=="after","live pair survives churn");
		offer->close();answer->close();local->close();remote->close();
		offer.reset();answer.reset();local.reset();remote.reset();
		wait([]{return managed_buffer_bytes()==0 && active_channels()==0 && active_peers()==0;},"final cleanup");
		// a different bind succeeds only once the backend closed the shared socket
		wait([&]{try{Peer::create(mux(std::nullopt,port))->close();return true;}catch(const Error& e){if(e.code!=1){throw;}return false;}},"shared socket release");
		stop_dispatcher();
		std::cout<<"PASS live mux pair exchanged "<<exchanged<<" round trips while 100 agents joined and left its UDP socket concurrently\n";
	}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
