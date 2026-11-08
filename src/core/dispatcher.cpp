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
#include <cstdint>
#include <list>
#include <stdexcept>
#include <thread>

namespace amber::rtc {
namespace {
// at most two workers send at the same time because more made a flood slow every other peer down 20x (slow-100-mux)
// a worker stuck inside the backend stops counting, so a replacement takes its place
// a peer has at most one call in flight, so with n connected peers at most n - 1 workers can be stuck while another peer waits,
// the thread count is capped at concurrent + n and a healthy peer always finds a worker, threads only exist after a stall
// a peer closed during its stuck call no longer counts as connected but its worker stays stuck, so those add to the cap
class Dispatcher{
	static constexpr size_t concurrent = 2;
	// only reachable with this many backend calls stuck for good, far beyond any connection count
	static constexpr size_t hardCap = 1024;
	static constexpr auto stallAfter = std::chrono::milliseconds{25};
	static constexpr auto retireAfter = std::chrono::seconds{2};
	static constexpr auto watchEvery = std::chrono::milliseconds{10};

	struct Worker{
		std::thread thread;
		std::atomic<std::int64_t> callSince{0};
		std::atomic<bool> stalled{false};
		bool servicing = false;
		Peer* peer = nullptr;
		// workers born from a stall leave after a while without work, the first ones stay
		bool extra = false, retired = false;
	};

	std::mutex mutex;
	std::condition_variable ready, watch;
	Peer* head = nullptr;
	Peer* tail = nullptr;
	bool stopping = false;
	size_t running = 0;
	std::atomic<size_t> inCalls{0};
	std::list<Worker> workers;
	size_t retiredCount = 0;
	bool reap = false;
	std::thread supervisor;

	static std::int64_t now_ns(){
		return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	// mutex held, a failed thread start leaves the dispatcher with the workers it has
	void spawn(bool extra) noexcept {
		try{
			workers.emplace_back();
			auto* worker = &workers.back();
			worker->extra = extra;
			try{
				worker->thread = std::thread([this, worker]{ run(*worker); });
			} catch(...){
				workers.pop_back();
			}
		} catch(...){}
	}

	void run(Worker& self) noexcept {
		current = &self;
		for(;;){
			std::shared_ptr<Peer> peer;
			{
				std::unique_lock lock(mutex);
				if(self.extra){
					if(!ready.wait_for(lock, retireAfter, [&]{ return head || stopping; })){
						// the supervisor joins the thread and drops the slot
						self.retired = true;
						++retiredCount;
						reap = true;
						watch.notify_one();
						return;
					}
				} else{
					ready.wait(lock, [&]{ return head || stopping; });
				}
				if(!head){ return; }
				if(running >= concurrent && !stopping){ ready.wait(lock); continue; }
				auto now = std::chrono::steady_clock::now();
				auto deadline = std::chrono::steady_clock::time_point::max();
				Peer* next = nullptr;
				Peer* before = nullptr;
				Peer* previous = nullptr;
				for(auto* p = head; p; p = p->dispatchNext){
					// a peer already serviced by another worker stays queued until that worker finishes
					if(!p->dispatchRunning){
						if(p->dispatchAt <= now){ next = p; before = previous; break; }
						deadline = (std::min)(deadline, p->dispatchAt);
					}
					previous = p;
				}
				if(!next){
					if(deadline == std::chrono::steady_clock::time_point::max()){ ready.wait(lock); }
					else{ ready.wait_until(lock, deadline); }
					continue;
				}
				if(before){ before->dispatchNext = next->dispatchNext; } else{ head = next->dispatchNext; }
				if(tail == next){ tail = before; }
				next->dispatchNext = nullptr;
				next->dispatchRunning = true;
				peer = std::move(next->dispatchHold);
				self.servicing = true;
				self.peer = next;
				self.stalled = false;
				++running;
			}
			peer->service();
			bool more, halting;
			{
				std::lock_guard lock(mutex);
				peer->dispatchRunning = false;
				self.servicing = false;
				self.peer = nullptr;
				if(!self.stalled.exchange(false)){ --running; }
				more = static_cast<bool>(peer->dispatchHold) || head;
				halting = stopping;
			}
			// while stopping every parked worker has to look again, the queue may just have drained
			if(halting){ ready.notify_all(); } else if(more){ ready.notify_one(); }
		}
	}

	void supervise() noexcept {
		std::unique_lock lock(mutex);
		for(;;){
			watch.wait(lock, [&]{ return stopping || reap || inCalls.load() != 0; });
			if(stopping){ return; }
			if(reap){
				for(auto it = workers.begin(); it != workers.end();){
					if(it->retired){
						if(it->thread.joinable()){ it->thread.join(); }
						it = workers.erase(it);
						--retiredCount;
					} else{ ++it; }
				}
				reap = false;
				if(inCalls.load() == 0){ continue; }
			}
			watch.wait_for(lock, watchEvery, [&]{ return stopping; });
			if(stopping){ return; }
			const auto limit = now_ns() - std::chrono::duration_cast<std::chrono::nanoseconds>(stallAfter).count();
			bool released = false;
			size_t idle = 0, orphans = 0;
			for(auto& worker : workers){
				const auto since = worker.callSince.load();
				if(worker.servicing && !worker.stalled && since != 0 && since <= limit){
					worker.stalled = true;
					--running;
					released = true;
				}
				if(!worker.servicing && !worker.retired){ ++idle; }
				if(worker.servicing && worker.stalled && worker.peer && worker.peer->dispatchStopped){ ++orphans; }
			}
			if(released){
				if(idle == 0 && workers.size() - retiredCount < (std::min)(hardCap, concurrent + active_peers() + orphans)){ spawn(true); }
				ready.notify_all();
			}
		}
	}

public:
	static thread_local Worker* current;

	Dispatcher(){
		std::unique_lock lock(mutex);
		for(size_t i = 0; i < concurrent; ++i){ spawn(false); }
		if(workers.empty()){ throw std::runtime_error("Unable to start the dispatcher workers"); }
		try{
			supervisor = std::thread([this]{ supervise(); });
		} catch(...){
			stopping = true;
			lock.unlock();
			ready.notify_all();
			for(auto& worker : workers){ if(worker.thread.joinable()){ worker.thread.join(); } }
			throw;
		}
	}
	~Dispatcher(){ stop(); }
	void stop() noexcept {
		{
			std::lock_guard lock(mutex);
			stopping = true;
		}
		ready.notify_all();
		watch.notify_all();
		// the supervisor goes first so no worker is spawned while the others are joined
		if(supervisor.joinable()){ supervisor.join(); }
		for(auto& worker : workers){
			if(worker.thread.joinable()){ worker.thread.join(); }
		}
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
	size_t worker_count(){
		std::lock_guard lock(mutex);
		return workers.size() - retiredCount;
	}
	size_t stalled_count(){
		std::lock_guard lock(mutex);
		size_t count = 0;
		for(auto& worker : workers){ if(worker.stalled) { ++count; } }
		return count;
	}
	void call_begin() noexcept {
		if(!current){ return; }
		current->callSince = now_ns();
		if(inCalls.fetch_add(1) == 0){
			// the empty section orders the increment before the supervisor sleeps on it
			{ std::lock_guard lock(mutex); }
			watch.notify_one();
		}
	}
	void call_end() noexcept {
		if(!current){ return; }
		current->callSince = 0;
		inCalls.fetch_sub(1);
		// a worker that was replaced counts again, the pickup limit absorbs the overlap
		if(current->stalled.exchange(false)){
			std::lock_guard lock(mutex);
			++running;
		}
	}
};

thread_local Dispatcher::Worker* Dispatcher::current = nullptr;

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
size_t dispatcher_workers() noexcept { auto* p = started.load(); return p ? p->worker_count() : 0; }
size_t dispatcher_stalled() noexcept { auto* p = started.load(); return p ? p->stalled_count() : 0; }
void backend_call_begin() noexcept { if(auto* p = started.load()){ p->call_begin(); } }
void backend_call_end() noexcept { if(auto* p = started.load()){ p->call_end(); } }

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
