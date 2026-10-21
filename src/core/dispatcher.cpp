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

#include "dispatcher.h"
#include "../channel/channel.h"
#include <condition_variable>
#include <thread>

namespace amber::rtc {
namespace {
class Dispatcher{
	std::mutex mutex;
	std::condition_variable ready;
	Peer* head = nullptr;
	Peer* tail = nullptr;
	bool stopping = false;
	std::thread worker;

	void run() noexcept {
		for(;;){
			std::shared_ptr<Peer> peer;
			{
				std::unique_lock lock(mutex);
				ready.wait(lock, [&]{ return head || stopping; });
				if(!head){ return; }
				auto now = std::chrono::steady_clock::now();
				auto deadline = std::chrono::steady_clock::time_point::max();
				Peer* next = nullptr;
				Peer* before = nullptr;
				Peer* previous = nullptr;
				for(auto* p = head; p; p = p->dispatchNext){
					if(p->dispatchAt <= now){ next = p; before = previous; break; }
					deadline = (std::min)(deadline, p->dispatchAt);
					previous = p;
				}
				if(!next){ ready.wait_until(lock, deadline); continue; }
				if(before){ before->dispatchNext = next->dispatchNext; } else{ head = next->dispatchNext; }
				if(tail == next){ tail = before; }
				next->dispatchNext = nullptr;
				peer = std::move(next->dispatchHold);
			}
			peer->service();
		}
	}

public:
	Dispatcher() : worker([this]{ run(); }){}
	~Dispatcher(){ stop(); }
	void stop() noexcept {
		{
			std::lock_guard lock(mutex);
			stopping = true;
		}
		ready.notify_one();
		if(worker.joinable()){ worker.join(); }
	}
	void enqueue(const std::shared_ptr<Peer>& peer, std::chrono::milliseconds delay) noexcept {
		{
			std::lock_guard lock(mutex);
			if(stopping || peer->dispatchStopped){ return; }
			auto at = std::chrono::steady_clock::now() + delay;
			if(peer->dispatchHold){
				peer->dispatchAt = (std::min)(peer->dispatchAt, at);
				ready.notify_one();
				return;
			}
			peer->dispatchAt = at;
			peer->dispatchHold = peer;
			if(tail){ tail->dispatchNext = peer.get(); } else{ head = peer.get(); }
			tail = peer.get();
		}
		ready.notify_one();
	}
};

std::atomic<Dispatcher*> started{nullptr};

Dispatcher& dispatcher(){
	static Dispatcher instance;
	return instance;
}
}

void start_dispatcher(){ started = &dispatcher(); }
void stop_dispatcher() noexcept { if(auto* p = started.load()){ p->stop(); } }
void schedule(const std::shared_ptr<Peer>& peer) noexcept {
	if(auto* p = started.load()){ p->enqueue(peer, std::chrono::milliseconds{0}); }
}
void schedule_after(const std::shared_ptr<Peer>& peer, std::chrono::milliseconds delay) noexcept {
	if(auto* p = started.load()){ p->enqueue(peer, delay); }
}

void Peer::service() noexcept {
	try{
		std::lock_guard operation(operations);
		std::array<std::shared_ptr<Channel>, 16> children;
		{
			std::lock_guard lock(mutex);
			if(closed){ return; }
			children = channels;
		}
		for(auto& c : children){
			if(c && c->closeRequested){ c->close(); }
		}
		dispatch_sends();
	} catch(...){
		{
			std::lock_guard lock(mutex);
			if(!closed){
				failure = ::rtc::PeerConnection::State::Failed;
				state = *failure;
			}
		}
		close_safely();
	}
}
}
