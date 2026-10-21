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
#include <dlfcn.h>
#include <sys/socket.h>
#include <cerrno>
struct socket;
std::atomic<bool> blockSctp{false};
extern "C" ssize_t usrsctp_sendv(struct socket* socket,const void* data,size_t size,struct sockaddr* to,int count,void* info,socklen_t length,unsigned int type,int flags){
	using Send=decltype(&usrsctp_sendv);
	static auto real=reinterpret_cast<Send>(dlsym(RTLD_NEXT,"usrsctp_sendv"));
	if(blockSctp){errno=EWOULDBLOCK;return -1;}
	return real(socket,data,size,to,count,info,length,type,flags);
}
using namespace amber::rtc;
using namespace std::chrono_literals;
void check(bool value,const char* why){if(!value){throw std::runtime_error(why);}}
template<class F> void wait(F f){auto end=std::chrono::steady_clock::now()+10s;while(!f()){if(std::chrono::steady_clock::now()>end){throw std::runtime_error("cross-channel credit deadline");}std::this_thread::sleep_for(2ms);}}
std::optional<std::string> read(const std::shared_ptr<Channel>& channel){auto size=channel->read_size();if(!size){return {};}std::string result(*size,'\0');return channel->copy_front(result.data(),*size,true)?std::optional(result):std::nullopt;}
int main(){
	try{
		Options options;options.message=8;options.sendBytes=8;options.pending=4;
		auto a=Peer::create(options),b=Peer::create(options);
		auto first=a->create_channel("A",{}),second=a->create_channel("B",{});
		wait([&]{a->snapshot();std::lock_guard lock(a->mutex);return a->gathering==::rtc::PeerConnection::GatheringState::Complete;});
		b->remote(*a->localDescription,true);
		wait([&]{b->snapshot();std::lock_guard lock(b->mutex);return b->gathering==::rtc::PeerConnection::GatheringState::Complete;});
		a->remote(*b->localDescription,false);
		std::shared_ptr<Channel> rxA,rxB;
		wait([&]{for(auto c:b->poll_channels()){if(c->label=="A"){rxA=c;}else{rxB=c;}}return rxA && rxB && first->is_open() && second->is_open();});
		blockSctp=true;check(first->send(""),"empty admission");
		wait([&]{std::lock_guard lock(a->queues->mutex);return a->queues->submitted==1;});
		check(second->send("12345678"),"second channel admission");
		wait([&]{std::lock_guard lock(a->queues->mutex);return a->queues->waiting && a->queues->submitted==2;});
		check(!first->send("x"),"aggregate TX bytes ignored");
		blockSctp=false;
		// a real control send supplies the socket event suppressed by fault injection
		auto control=a->create_channel("control",{});
		wait([&]{std::lock_guard lock(a->queues->mutex);return a->queues->sendMessages==0;});
		wait([&]{return rxA->queued_messages()==1 && rxB->queued_messages()==1;});
		check(read(rxA)=="" && read(rxB)=="12345678","nonempty drain witness corrupts earlier empty debt");
		blockSctp=true;check(first->send(""),"second empty admission");
		wait([&]{std::lock_guard lock(a->queues->mutex);return a->queues->submitted==1;});
		blockSctp=false;check(second->send("witness"),"direct witness admission");
		wait([&]{std::lock_guard lock(a->queues->mutex);return a->queues->sendMessages==0;});
		wait([&]{return rxA->queued_messages()==1 && rxB->queued_messages()==1;});
		check(read(rxA)=="" && read(rxB)=="witness" && !read(rxA) && !read(rxB),"true witness duplicated or lost data");
		a->close();b->close();stop_dispatcher();
		std::cout<<"PASS aggregate TX and cross-channel empty debt retirement by nonempty drain and true FIFO witness\n";
	}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
