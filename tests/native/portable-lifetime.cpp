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
#include <condition_variable>
#include <future>
#include <iostream>
#include <thread>

using namespace amber::rtc;
using namespace std::chrono_literals;

void require(bool value, const char* message){ if(!value){ throw std::runtime_error(message); } }
template<class F> void until(F ready){
	const auto deadline = std::chrono::steady_clock::now() + 10s;
	while(!ready()){
		if(std::chrono::steady_clock::now() > deadline){ throw std::runtime_error("portable lifetime deadline"); }
		std::this_thread::yield();
	}
}
size_t budget_used(){
	size_t low = 0, high = bufferLimit;
	while(low < high){
		const auto amount = low + (high - low + 1) / 2;
		try{ reserve_escrow(amount); release_escrow(amount); low = amount; }
		catch(const Error& error){ if(error.code != 10){ throw; } high = amount - 1; }
	}
	return bufferLimit - low;
}
struct Gate{
	std::mutex mutex;
	std::condition_variable ready;
	bool entered = false, permit = false;
	void release(){ std::lock_guard lock(mutex); permit = true; ready.notify_all(); }
};
struct Owner{
	std::shared_ptr<Escrow> credit;
	Gate& gate;
	Owner(std::shared_ptr<Escrow> value, Gate& state) : credit(std::move(value)), gate(state){}
	~Owner(){
		std::unique_lock lock(gate.mutex);
		gate.entered = true;
		gate.ready.notify_all();
		gate.ready.wait(lock, [&]{ return gate.permit; });
	}
};
int main(){
	Gate gate;
	std::future<void> sender;
	try{
		auto a = Peer::create({}), b = Peer::create({});
		auto local = a->create_channel("portable", {});
		until([&]{ a->snapshot(); std::lock_guard lock(a->mutex); return a->gathering == ::rtc::PeerConnection::GatheringState::Complete; });
		b->remote(*a->localDescription, true);
		until([&]{ b->snapshot(); std::lock_guard lock(b->mutex); return b->gathering == ::rtc::PeerConnection::GatheringState::Complete; });
		a->remote(*b->localDescription, false);
		std::shared_ptr<Channel> remote;
		until([&]{ for(auto c : b->poll_channels()){ remote = c; } return remote && local->is_open(); });
		require(local->send("first"), "native admission failed");
		until([&]{ return remote->queued_messages() == 1; });
		char received[5];
		require(remote->copy_front(received, 5, true) && std::string(received, 5) == "first", "native payload changed");
		until([&]{ std::lock_guard lock(a->queues->mutex); return a->queues->sendMessages == 0; });
		std::unique_lock operation(a->operations);
		require(local->send("controlled"), "controlled admission failed");
		std::shared_ptr<Escrow> credit;
		Buffer* payload = nullptr;
		size_t escrow = 0;
		{
			std::lock_guard lock(a->queues->mutex);
			credit = a->queues->sends[a->queues->first].credit;
			payload = a->queues->sends[a->queues->first].payload;
			escrow = a->queues->sends[a->queues->first].escrow;
		}
		auto backend = local->backend;
		std::weak_ptr<Escrow> weak = credit;
		auto owner = std::make_shared<Owner>(std::move(credit), gate);
		sender = std::async(std::launch::async, [backend, payload, owner = std::move(owner)]() mutable {
			backend->send(reinterpret_cast<const std::byte*>(payload->data()), payload->size, std::move(owner));
		});
		{
			std::unique_lock lock(gate.mutex);
			require(gate.ready.wait_for(lock, 10s, [&]{ return gate.entered; }), "backend final owner was not invoked");
		}
		a->close(); a->close(); b->close();
		require(!weak.expired(), "close released live native owner");
		require(budget_used() == managed_buffer_bytes() + escrow, "purge released shared escrow early");
		const auto filler = bufferLimit - budget_used();
		reserve_escrow(filler);
		bool rejected = false;
		try{ reserve_escrow(1); } catch(const Error& error){ require(error.code == 10, "unexpected budget error"); rejected = true; }
		require(rejected, "global budget reused while owner remained");
		release_escrow(filler);
		gate.release();
		sender.get();
		require(weak.expired(), "final native owner retained credit");
		auto failed = make_escrow(128);
		std::weak_ptr<Escrow> failedWeak = failed;
		bool closed = false;
		try{ const std::byte payload{0}; backend->send(&payload, 1, std::move(failed)); }
		catch(const std::runtime_error&){ closed = true; }
		require(closed && failedWeak.expired(), "closed backend send retained credit");
		operation.unlock();
		backend.reset(); remote.reset(); local.reset(); a.reset(); b.reset();
		until([]{ return budget_used() == 0 && managed_buffer_bytes() == 0 && active_peers() == 0 && active_channels() == 0; });
		stop_dispatcher();
		std::cout << "PASS portable native final-owner close, budget and exception release\n";
	}catch(const std::exception& error){
		gate.release();
		if(sender.valid()){ sender.wait(); }
		std::cerr << error.what() << '\n';
		return 1;
	}
	return 0;
}
