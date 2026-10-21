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

#include "channel.h"
#include "../core/dispatcher.h"
#include <cstring>

namespace amber::rtc {

void Channel::incoming(std::string_view payload) noexcept {
	bool overflow = false;
	bool resourceFailure = false;
	auto p = parent.lock();
	if(!p){ return; }
	{
		std::lock_guard lock(queues->mutex);
		if(closed || receiveError){ return; }
		if(payload.size() > p->options.message ||
			payload.size() > p->options.receiveBytes - queues->receiveBytes ||
			queues->receiveMessages >= p->options.receiveMessages){
			overflow = true;
		} else{
			try{
				auto* entry = Buffer::copy(payload);
				if(receiveTail){ receiveTail->next = entry; } else{ receiveHead = entry; }
				receiveTail = entry;
				receiveBytes += payload.size();
				++receiveMessages;
				queues->receiveBytes += payload.size();
				++queues->receiveMessages;
			} catch(...){
				overflow = true;
				resourceFailure = true;
			}
		}
		if(overflow){
			saturating_increment(queues->overflows);
			count_overflow();
			const bool reliable = !settings.reliability.maxRetransmits && !settings.reliability.maxPacketLifeTime;
			if(reliable || resourceFailure){
				receiveError = reliable ? 9 : 10;
				closeRequested = true;
			}
		}
	}
	if(closeRequested){
		if(auto p = parent.lock()){ schedule(p); }
	}
}

void Channel::purge_receive() noexcept {
	if(!queues){ return; }
	std::lock_guard lock(queues->mutex);
	while(receiveHead){
		auto* entry = receiveHead;
		receiveHead = entry->next;
		Buffer::release(entry);
	}
	receiveTail = nullptr;
	queues->receiveBytes -= receiveBytes;
	queues->receiveMessages -= receiveMessages;
	receiveBytes = receiveMessages = 0;
}

std::optional<size_t> Channel::read_size(){
	std::lock_guard lock(queues->mutex);
	if(receiveError){ throw Error(receiveError, "Receive admission failed"); }
	if(closed || !receiveHead){ return {}; }
	return receiveHead->size;
}

bool Channel::copy_front(char* target, size_t size, bool consume){
	std::lock_guard lock(queues->mutex);
	if(receiveError){ throw Error(receiveError, "Receive admission failed"); }
	if(closed || !receiveHead || receiveHead->size != size){ return false; }
	if(size){ std::memcpy(target, receiveHead->data(), size); }
	if(consume){
		auto* entry = receiveHead;
		receiveHead = entry->next;
		if(!receiveHead){ receiveTail = nullptr; }
		receiveBytes -= size;
		--receiveMessages;
		queues->receiveBytes -= size;
		--queues->receiveMessages;
		Buffer::release(entry);
	}
	return true;
}

size_t Channel::available_bytes(){
	std::lock_guard lock(queues->mutex);
	return receiveBytes;
}

size_t Channel::queued_messages(){
	std::lock_guard lock(queues->mutex);
	return receiveMessages;
}
}
