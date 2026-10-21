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
#include <dlfcn.h>
#include <iostream>
#include <source_location>
#include <sys/socket.h>
#include <thread>

struct socket;
std::atomic<bool> blockSctp{false};
std::atomic<bool> failSctp{false};
std::atomic<size_t> blockedCalls{0};
thread_local bool closeAfterFalse = false;
thread_local bool closeBeforeSend = false;
thread_local bool closeAfterError = false;
std::atomic<bool> gateReset{false}, resetPaused{false}, releaseReset{false};
extern "C" bool remove_channel(void*, uint16_t) asm("_ZN3rtc4impl14PeerConnection17removeDataChannelEt");
extern "C" bool remove_channel(void* peer, uint16_t stream){
	using Remove = bool(*)(void*, uint16_t);
	static auto real = reinterpret_cast<Remove>(dlsym(RTLD_NEXT, "_ZN3rtc4impl14PeerConnection17removeDataChannelEt"));
	if(!real){ throw std::runtime_error("real removeDataChannel missing"); }
	const bool removed = real(peer, stream);
	if(gateReset.exchange(false)){
		resetPaused = true;
		while(!releaseReset.load()){ releaseReset.wait(false); }
	}
	return removed;
}
extern "C" ssize_t usrsctp_sendv(struct socket* socket, const void* data, size_t size,
	struct sockaddr* to, int count, void* info, socklen_t length, unsigned int type, int flags){
	using Send = decltype(&usrsctp_sendv);
	static auto real = reinterpret_cast<Send>(dlsym(RTLD_NEXT, "usrsctp_sendv"));
	if(failSctp){ errno = EIO; return -1; }
	if(blockSctp){ ++blockedCalls; errno = EWOULDBLOCK; return -1; }
	return real(socket, data, size, to, count, info, length, type, flags);
}

bool rtc::DataChannel::send(const byte* data, size_t size, std::shared_ptr<void> lifetime){
	using Send = bool(*)(DataChannel*, const byte*, size_t, std::shared_ptr<void>);
	static auto real = reinterpret_cast<Send>(dlsym(RTLD_NEXT, "_ZN3rtc11DataChannel4sendEPKSt4bytemSt10shared_ptrIvE"));
	if(!real){ throw std::runtime_error("real DataChannel::send missing"); }
	if(std::exchange(closeBeforeSend, false)){ close(); }
	bool direct;
	try{
		direct = real(this, data, size, std::move(lifetime));
	} catch(...){
		if(std::exchange(closeAfterError, false)){ close(); }
		throw;
	}
	// deliver the real closed callback in the handoff return window
	if(!direct && std::exchange(closeAfterFalse, false)){ close(); }
	return direct;
}

using namespace amber::rtc;
using namespace std::chrono_literals;
void check(bool value, const char* why){ if(!value){ throw std::runtime_error(why); } }
template<class F> void wait(F ready, std::source_location where = std::source_location::current()){
	auto end = std::chrono::steady_clock::now() + 10s;
	while(!ready()){
		if(std::chrono::steady_clock::now() > end){
			throw std::runtime_error("credit close deadline at line " + std::to_string(where.line()));
		}
		std::this_thread::sleep_for(2ms);
	}
}
std::optional<std::string> read(const std::shared_ptr<Channel>& channel){
	auto size = channel->read_size();
	if(!size){ return {}; }
	std::string result(*size, '\0');
	return channel->copy_front(result.data(), *size, true) ? std::optional(result) : std::nullopt;
}

void run(const std::string& mode){
	Options options; options.message = 64; options.sendBytes = 64; options.pending = 4;
	auto a = Peer::create(options), b = Peer::create(options);
	auto first = a->create_channel("A", {}), second = a->create_channel("B", {});
	auto sibling = a->create_channel("C", {});
	wait([&]{ a->snapshot(); std::lock_guard lock(a->mutex); return a->gathering == ::rtc::PeerConnection::GatheringState::Complete; });
	b->remote(*a->localDescription, true);
	wait([&]{ b->snapshot(); std::lock_guard lock(b->mutex); return b->gathering == ::rtc::PeerConnection::GatheringState::Complete; });
	a->remote(*b->localDescription, false);
	std::shared_ptr<Channel> rxA, rxB, rxC;
	wait([&]{
		for(auto c : b->poll_channels()){
			if(c->label == "A"){ rxA = c; }
			if(c->label == "B"){ rxB = c; }
			if(c->label == "C"){ rxC = c; }
		}
		return rxA && rxB && rxC && first->is_open() && second->is_open() && sibling->is_open();
	});
	{
		// exclude the worker so every assertion precedes the next backend admission
		std::lock_guard operation(a->operations);
		blockSctp = true;
		check(first->send(""), "A empty admission rejected");
		a->service();
		struct ResetRelease{
			~ResetRelease(){ releaseReset = true; releaseReset.notify_all(); }
		} resetRelease;
		if(mode == "unregister"){
			gateReset = true;
			rxB->close();
			wait([]{ return resetPaused.load(); });
		}
		closeAfterFalse = mode == "during-send";
		closeBeforeSend = mode == "before-send";
		check(second->send("pending-B"), "B admission rejected");
		if(mode == "failure-close"){ failSctp = true; closeAfterError = true; }
		a->service();
		if(mode == "failure-close"){
			failSctp = false;
			std::lock_guard lock(a->mutex);
			check(a->closed && a->failure == ::rtc::PeerConnection::State::Failed, "real backend error hidden by channel closure");
			check(a->queues->sendMessages == 0, "failed peer kept credits");
			blockSctp = false;
			b->close();
			return;
		}
		if(mode == "unregister"){
			bool retained;
			{
				std::lock_guard lock(a->queues->mutex);
				retained = a->queues->submitted == 2 && a->queues->sendMessages == 2 && a->queues->waiting;
			}
			releaseReset = true; releaseReset.notify_all();
			check(retained, "unregistered B zero bufferedAmount prematurely retired A/B credits");
			wait([&]{ return second->is_closed(); });
			a->service();
		}
		size_t escrowA, escrowB;
		{
			std::lock_guard lock(a->queues->mutex);
			auto& q = *a->queues;
			check(blockedCalls > 0 && q.submitted == 2 && q.sendMessages == 2, "false/close retired credits during handoff");
			check(q.waiting == (mode != "during-send" && mode != "before-send" && mode != "unregister"), "missing real false/waiting setup");
			escrowA = q.sends[q.first].escrow;
			escrowB = q.sends[(q.first + 1) % q.sends.size()].escrow;
			check(escrowA > 0 && escrowB > 0, "backend escrow not reserved");
			check(q.sendBytes == 9 && first->sendBytes == 0 && second->sendBytes == 9, "initial byte accounting");
		}
		if(mode == "disconnect"){
			auto transport = a->backend;
			b->close();
			wait([&]{ return transport->state() != ::rtc::PeerConnection::State::Connected; });
			a->service();
			{
				std::lock_guard lock(a->mutex);
				check(a->closed && a->failure == ::rtc::PeerConnection::State::Failed, "false then disconnect hid shared transport failure");
			}
			check(a->queues->sendMessages == 0 && a->queues->submitted == 0, "disconnect leaked ledger entries");
			blockSctp = false;
			return;
		}
		if(mode == "queued"){ check(second->send("cancel-B"), "queued B admission rejected"); }
		if(mode == "remote"){
			rxB->close();
			wait([&]{ return second->is_closed(); });
		}else{
			second->close();
		}
		if(mode == "both"){ first->close(); }
		a->service();
		{
			std::lock_guard lock(a->queues->mutex);
			auto& q = *a->queues;
			const size_t expected = mode == "queued" ? 3 : 2;
			const size_t bytes = mode == "queued" ? 17 : 9;
			check(q.sendMessages == expected && q.submitted == expected, "closing B prematurely retired A/B backend credits");
			check(q.sendBytes == bytes && second->sendBytes == bytes, "closing B released pending bytes");
			auto& entryA = q.sends[q.first];
			auto& entryB = q.sends[(q.first + 1) % q.sends.size()];
			check(entryA.channel == first && entryB.channel == second, "closing B released ledger ownership");
			check(entryA.escrow == escrowA && entryB.escrow == escrowB, "closing B released backend escrow");
			check(!entryA.payload && !entryB.payload, "handoff retained owned copies");
			if(mode == "queued"){
				auto& cancelled = q.sends[(q.first + 2) % q.sends.size()];
				check(cancelled.channel == second && !cancelled.payload && cancelled.escrow > 0, "cancelled B retired older debt");
			}
			check(!q.waiting, "closed witness blocks sibling progress");
		}
		{
			std::lock_guard lock(a->mutex);
			check(!a->closed && a->failure != ::rtc::PeerConnection::State::Failed, "channel close failed healthy peer");
		}
		check(sibling->is_open() && (mode == "both" || first->is_open()), "sibling closed");
		if(mode == "teardown"){
			a->close();
			check(a->queues->sendMessages == 0 && a->queues->submitted == 0 && a->queues->sendBytes == 0, "peer close leaked credits");
		}else{
			if(mode == "direct"){ blockSctp = false; }
			check(sibling->send("held-C"), "C admission rejected");
			a->service();
			{
				std::lock_guard lock(a->queues->mutex);
				if(mode == "direct"){
					check(a->queues->sendMessages == 0 && a->queues->submitted == 0 && a->queues->sendBytes == 0, "true send did not retire FIFO prefix");
				}else{
					const size_t expected = mode == "queued" ? 4 : 3;
					check(a->queues->submitted == expected && a->queues->sendMessages == expected && a->queues->waiting, "another false released older debt");
					check(a->queues->sendBytes == (mode == "queued" ? 23 : 15), "aggregate retained bytes wrong");
				}
			}
			if(mode != "direct"){ check(!read(rxA) && !read(rxC), "congestion failed to hold payloads"); }
			blockSctp = false;
			// a control send supplies the socket event suppressed by fault injection
			auto control = a->create_channel("drain-control", {});
			wait([&]{ a->service(); std::lock_guard lock(a->queues->mutex); return a->queues->sendMessages == 0; });
			check(second->buffered_bytes() == 0 && sibling->buffered_bytes() == 0, "drained bytes retained");
			wait([&]{ return rxC->queued_messages() == 1 && (mode == "both" || rxA->queued_messages() == 1); });
			if(mode != "both"){ check(read(rxA) == "" && !read(rxA), "A empty lost or duplicated"); }
			check(read(rxC) == "held-C" && !read(rxC), "C lost, duplicated or corrupt");
			check(rxC->send("reply-C"), "C reverse admission rejected");
			wait([&]{ b->service(); return sibling->queued_messages() == 1; });
			check(read(sibling) == "reply-C" && !read(sibling), "C reverse lost, duplicated or corrupt");
			if(mode != "both"){
				check(first->send("live-A"), "A no longer operational");
				wait([&]{ a->service(); return rxA->queued_messages() == 1; });
				check(read(rxA) == "live-A" && !read(rxA), "A live payload corrupt");
			}
		}
	}
	blockSctp = false;
	a->close(); b->close();
	check(a->queues->sendMessages == 0 && a->queues->sendBytes == 0 && a->queues->submitted == 0, "final credit leak or underflow");
}

int main(int argc, char** argv){
	try{
		run(argc == 2 ? argv[1] : "local");
		wait([]{ return managed_buffer_bytes() == 0 && active_channels() == 0 && active_peers() == 0; });
		// the whole budget must be reusable, including escrow excluded from owned metrics
		reserve_escrow(bufferLimit); release_escrow(bufferLimit);
		stop_dispatcher();
		std::cout << "PASS channel close retains cross-channel escrow and ownership until FIFO witness or peer close\n";
	}catch(const std::exception& e){
		std::cerr << e.what() << '\n';
		return 1;
	}
}
