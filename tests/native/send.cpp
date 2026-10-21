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
#include <source_location>
#include <cstdlib>
#include <new>
namespace { thread_local bool failNextAllocation=false; }
void* operator new(size_t size){if(failNextAllocation){failNextAllocation=false;throw std::bad_alloc();}if(auto* p=std::malloc(size?size:1)){return p;}throw std::bad_alloc();}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,size_t) noexcept {std::free(p);}
#ifdef __linux__
#include <dlfcn.h>
#include <sys/socket.h>
#include <cerrno>
std::atomic<bool> blockSctp{false}, failSctp{false};
std::atomic<size_t> blockedCalls{0};
struct socket;
extern "C" ssize_t usrsctp_sendv(struct socket* socket, const void* data, size_t length, struct sockaddr* to,
	int count, void* info, socklen_t infoLength, unsigned int type, int flags){
	using Send = decltype(&usrsctp_sendv);
	static auto real = reinterpret_cast<Send>(dlsym(RTLD_NEXT,"usrsctp_sendv"));
	if(failSctp){ errno=EIO; return -1; }
	if(blockSctp){ ++blockedCalls; errno=EWOULDBLOCK; return -1; }
	return real(socket,data,length,to,count,info,infoLength,type,flags);
}
#endif
using namespace amber::rtc;
using namespace std::chrono_literals;
void check(bool v,const char* why){ if(!v){ throw std::runtime_error(why); } }
template<class F> void wait(F f, std::source_location where=std::source_location::current()){ auto end=std::chrono::steady_clock::now()+10s; while(!f()){ if(std::chrono::steady_clock::now()>end){ throw std::runtime_error("send deadline at line "+std::to_string(where.line())); } std::this_thread::sleep_for(2ms); } }
std::optional<std::string> read(const std::shared_ptr<Channel>& c){ auto size=c->read_size(); if(!size){ return {}; } std::string result(*size,'\0'); return c->copy_front(result.data(),*size,true)?std::optional(result):std::nullopt; }
int main(int argc,char** argv){
	try{
		Options o; o.message=64; o.sendBytes=64; o.receiveMessages=512;
		auto a=Peer::create(o),b=Peer::create(o);
		auto channel=a->create_channel("transmit",{});
		auto sibling=a->create_channel("sibling",{});
		wait([&]{a->snapshot();return a->gathering==::rtc::PeerConnection::GatheringState::Complete;});
		b->remote(*a->localDescription,true);
		wait([&]{b->snapshot();return b->gathering==::rtc::PeerConnection::GatheringState::Complete;});
		a->remote(*b->localDescription,false);
		std::shared_ptr<Channel> incoming, incomingSibling;
		wait([&]{
			for(auto c : b->poll_channels()){
				if(c->label=="transmit"){ incoming=c; }
				if(c->label=="sibling"){ incomingSibling=c; }
			}
			return incoming && incomingSibling && channel->is_open() && sibling->is_open();
		});
		const auto baseline=managed_buffer_bytes();
		bool allocationFailed=false;
		failNextAllocation=true;
		try{channel->send("rollback");}catch(const std::bad_alloc&){allocationFailed=true;}
		check(allocationFailed && managed_buffer_bytes()==baseline && a->queues->sendMessages==0,"send allocation rollback");
#ifdef __linux__
		if(argc==2){
			const bool failure=std::string(argv[1])=="--backend-failure";
			failSctp=failure;blockSctp=!failure;
			check(channel->send("admitted"),"failure case not admitted");
			if(failure){
				wait([&]{return channel->is_closed();});
				std::lock_guard lock(a->mutex);
				check(a->failure==::rtc::PeerConnection::State::Failed,"backend failure was hidden");
			}else{
				wait([&]{std::lock_guard lock(a->queues->mutex);return a->queues->waiting && a->queues->sendMessages==1;});
				channel->close();
				wait([&]{return channel->is_closed();});
				{
					std::lock_guard lock(a->mutex);
					check(a->failure!=::rtc::PeerConnection::State::Failed,"channel close failed peer");
				}
				check(sibling->is_open(),"sibling channel closed prematurely");
				blockSctp=false;
				check(sibling->send("sibling-payload"),"send on sibling channel failed");
				wait([&]{return incomingSibling->queued_messages()==1;});
				check(read(incomingSibling)=="sibling-payload","sibling payload corrupted");
			}
			a->close();b->close();failSctp=false;blockSctp=false;
			check(a->queues->sendMessages==0 && a->queues->sendBytes==0,"failure close leaked reservations");
			std::cout<<"PASS backend failure or false followed by close retires in-flight ownership safely without failing sibling channels\n";
			return 0;
		}
#endif
		bool oversized=false;try{channel->send(std::string(65,'x'));}catch(const Error& e){oversized=e.code==6;}
		check(oversized,"oversize send admitted");
		std::string maximum(64,'x'); maximum[3]='\0';
		check(channel->send(maximum),"maximum send rejected");
		wait([&]{return incoming->queued_messages()==1;});
		check(read(incoming)==maximum,"binary transfer corrupt");
#ifdef __linux__
		blockSctp=true;
		check(channel->send(maximum),"admission false on backend capacity");
		wait([&]{return blockedCalls>0 && channel->buffered_bytes()==64;});
		check(!channel->send("x"),"exact TX byte limit ignored");
		check(!read(incoming),"injected EAGAIN did not prevent real submission");
		blockSctp=false;
		wait([&]{return incoming->queued_messages()==1 && channel->buffered_bytes()==0;});
		check(read(incoming)==maximum && !read(incoming),"false send retried or lost");
		blockSctp=true;blockedCalls=0;
		check(channel->send("") && channel->send(""),"empty FIFO witness setup");
		wait([&]{std::lock_guard lock(a->queues->mutex);return a->queues->submitted==2;});
		blockSctp=false;
		check(channel->send("witness"),"FIFO witness admission");
		wait([&]{std::lock_guard lock(a->queues->mutex);return a->queues->sendMessages==0;});
		wait([&]{return incoming->queued_messages()==3;});
		check(read(incoming)=="" && read(incoming)=="" && read(incoming)=="witness" && !read(incoming),"empty debt witness lost or duplicated messages");
		blockSctp=true; blockedCalls=0;
		for(int i=0;i<256;++i){check(channel->send(""),"empty slot rejected early");}
		wait([&]{std::lock_guard lock(a->queues->mutex);return blockedCalls>0 && a->queues->submitted==256;});
		check(!channel->send(""),"empty backlog escaped slot limit");
		check(channel->buffered_bytes()==0,"empty debt counted as payload bytes");
		blockSctp=false;
		auto drain = a->create_channel("drain-control", {});
		wait([&]{return incoming->queued_messages()==256;});
		check(!channel->send(""),"byte-only zero incorrectly retired empty debt");
		for(int i=0;i<256;++i){check(read(incoming)=="","empty payload missing or corrupt");}
		check(!read(incoming),"duplicate empty payload");
		drain.reset();
#endif
		a->close();b->close();
		check(a->queues->sendMessages==0 && a->queues->sendBytes==0,"TX close credit leak");
		incoming.reset(); channel.reset(); sibling.reset(); incomingSibling.reset(); a.reset(); b.reset();
		wait([]{return managed_buffer_bytes()==0;});
		stop_dispatcher();
		std::cout<<"PASS real binary send, maximum, backend EAGAIN, no retries, exact bytes and 256 empty debt slots\n";
	}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
