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

#include "buffers.h"
#include "../peer/peer.h"
#include <algorithm>
#include <cstring>
#include <rtc/message.hpp>
#include <limits>

namespace amber::rtc {
namespace {
std::atomic<size_t> managed{0}, budget{0}, overflows{0}, rejected{0}, sendUsed{0};
}

void reserve_escrow(size_t cost){
	auto used = budget.load();
	do{
		if(cost > bufferLimit - used){ throw Error(10, "Managed buffer budget exceeded"); }
	}while(!budget.compare_exchange_weak(used, used + cost));
}

void release_escrow(size_t cost) noexcept { budget.fetch_sub(cost); }

std::shared_ptr<Escrow> make_escrow(size_t bytes){
	ManagedCharge allocation(sizeof(Escrow) + 64);
	reserve_escrow(bytes);
	std::shared_ptr<Escrow> result;
	try{
		result = std::make_shared<Escrow>();
	} catch(...){
		release_escrow(bytes);
		throw;
	}
	result->allocation.swap(allocation);
	result->bytes = bytes;
	return result;
}

void ManagedCharge::resize(size_t amount){
	if(amount > bytes){
		reserve_escrow(amount - bytes);
		managed.fetch_add(amount - bytes);
	} else{
		managed.fetch_sub(bytes - amount);
		release_escrow(bytes - amount);
	}
	bytes = amount;
}

std::shared_ptr<Queues> make_queues(){
	ManagedCharge allocation(sizeof(Queues) + 64);
	auto result = std::make_shared<Queues>();
	result->allocation.swap(allocation);
	return result;
}

void saturating_increment(std::atomic<size_t>& value){
	auto n = value.load();
	const auto maximum = static_cast<size_t>((std::numeric_limits<intptr_t>::max)());
	while(n < maximum && !value.compare_exchange_weak(n, n + 1)){}
}

Buffer* Buffer::copy(std::string_view payload){
	const size_t bytes = payload.empty() ? 1 : payload.size();
	if(bytes > bufferLimit - sizeof(Buffer)){
		throw Error(10, "Managed buffer budget exceeded");
	}
	const size_t cost = sizeof(Buffer) + bytes;
	reserve_escrow(cost);
	managed.fetch_add(cost);
	try{
		auto* result = new(::operator new(cost)) Buffer;
		result->size = payload.size();
		result->charge = cost;
		if(!payload.empty()){
			std::memcpy(result->data(), payload.data(), payload.size());
		}
		return result;
	} catch(...){
		managed.fetch_sub(cost);
		release_escrow(cost);
		throw;
	}
}

void Buffer::release(Buffer* buffer) noexcept {
	if(buffer){
		const auto cost = buffer->charge;
		buffer->~Buffer();
		::operator delete(buffer);
		managed.fetch_sub(cost);
		release_escrow(cost);
	}
}

// the owned copy and the backend escrow are both charged
size_t send_cost(size_t payload){
	return 2 * (payload ? payload : 1) + sizeof(Buffer) + sizeof(::rtc::Message) + sizeof(Escrow) + 128;
}

// queued sends share one cap and above its floor a queue keeps to an equal part of the general pool
// an empty queue admits one message of any size from the general pool
// the global budget is checked too, so fixed peer charges and receives always keep headroom
bool reserve_send(size_t own, size_t cost, bool proven){
	const bool floor = own + cost <= sendFloor;
	if(!floor && own != 0 && own + cost > sendGeneral / (std::max)(active_peers(), sharePeers)){ return false; }
	const size_t limit = floor ? sendBudget : (proven ? sendGeneral : sendGeneral - provenHeadroom);
	const size_t global = floor ? bufferLimit - bufferLimit / 16 : bufferLimit - bufferLimit / 8;
	auto used = sendUsed.load();
	do{
		const auto spent = budget.load();
		const bool fits = used <= limit && cost <= limit - used && spent <= global && cost <= global - spent;
		// a message above any limit goes through only while nothing else is queued
		if(!fits && !(cost > limit && own == 0 && used == 0)){ return false; }
	}while(!sendUsed.compare_exchange_weak(used, used + cost));
	return true;
}

void release_send(size_t cost) noexcept { sendUsed.fetch_sub(cost); }

size_t send_footprint(){ return sendUsed.load(); }

size_t managed_buffer_bytes(){ return managed.load(); }
size_t budget_bytes(){ return budget.load(); }
size_t receive_overflow_count(){ return overflows.load(); }
size_t send_rejected_count(){ return rejected.load(); }
void count_overflow(){ saturating_increment(overflows); }
void count_rejection(){ saturating_increment(rejected); }
}
