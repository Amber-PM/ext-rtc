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
#include <rtc/message.hpp>
#include <limits>

namespace amber::rtc {
namespace {
struct Retired{
	std::array<std::shared_ptr<Channel>, 256> channels;
	size_t count = 0;
};

void retire(Queues& q, size_t count, Retired& retired) noexcept {
	while(count--){
		auto& entry = q.sends[q.first];
		q.sendBytes -= entry.size;
		entry.channel->sendBytes -= entry.size;
		Buffer::release(entry.payload);
		entry.credit.reset();
		retired.channels[retired.count++] = std::move(entry.channel);
		entry = {};
		q.first = (q.first + 1) % q.sends.size();
		--q.sendMessages;
		--q.submitted;
	}
}
}

bool Channel::send(std::string_view payload){
	auto peer = parent.lock();
	if(!peer){ throw Error(2, "Peer is closed"); }
	std::lock_guard operation(peer->operations);
	peer->require_open();
	std::lock_guard installation(lifecycle);
	if(closed || !backend){ throw Error(2, "Channel is closed"); }
	if(!backend->isOpen()){ throw Error(3, "Channel is not open"); }
	maximum = (std::min)(configuredMaximum, backend->maxMessageSize());
	if(payload.size() > maximum){ throw Error(6, "Payload exceeds channel capacity"); }
	{
		std::lock_guard lock(queues->mutex);
		if(closed){ throw Error(2, "Channel is closed"); }
		if(payload.size() > peer->options.sendBytes - queues->sendBytes ||
			queues->sendMessages == queues->sends.size()){
			saturating_increment(queues->rejected);
			count_rejection();
			return false;
		}
		// charge the owned copy and the backend handoff before publication
		const auto cost = (payload.empty() ? 1 : payload.size()) + sizeof(::rtc::Message) + 64;
		auto credit = make_escrow(cost);
		auto* copy = Buffer::copy(payload);
		auto& entry = queues->sends[(queues->first + queues->sendMessages) % queues->sends.size()];
		entry.channel = shared_from_this();
		entry.payload = copy;
		entry.size = payload.size();
		entry.escrow = cost;
		entry.credit = std::move(credit);
		queues->sendBytes += payload.size();
		sendBytes += payload.size();
		++queues->sendMessages;
	}
	schedule(peer);
	return true;
}

size_t Channel::buffered_bytes(){
	std::lock_guard lock(queues->mutex);
	return sendBytes;
}

void Peer::dispatch_sends(){
	Retired retired;
	auto& q = *queues;
	for(size_t work = 0; work < 32; ++work){
		std::shared_ptr<Channel> channel;
		Buffer* payload = nullptr;
		std::shared_ptr<Escrow> credit;
		bool waiting;
		size_t handedBytes;
		{
			std::lock_guard lock(q.mutex);
			waiting = q.waiting;
			handedBytes = q.handedBytes;
			if(waiting){
				channel = q.sends[(q.first + q.submitted - 1) % q.sends.size()].channel;
			} else{
				if(q.submitted == q.sendMessages){ return; }
				auto& entry = q.sends[(q.first + q.submitted) % q.sends.size()];
				channel = entry.channel;
				payload = entry.payload;
				credit = entry.credit;
			}
		}
		std::lock_guard installation(channel->lifecycle);
		auto transport = channel->backend;
		if(waiting){
			if(backend->state() != ::rtc::PeerConnection::State::Connected){
				throw Error(11, "Transport closed with pending backend backlog");
			}
			if(!transport || channel->closed){
				std::lock_guard lock(q.mutex);
				// stream close is not a FIFO drain witness
				q.waiting = false;
				continue;
			}
			// closed streams can disappear before their buffered count is updated
			if(backend->bytesSent() < handedBytes){
				schedule_after(shared_from_this(), std::chrono::milliseconds{5});
				return;
			}
			std::lock_guard lock(q.mutex);
			retire(q, q.submitted, retired);
			q.waiting = false;
			continue;
		}
		if(!transport || channel->closed){
			std::lock_guard lock(q.mutex);
			++q.submitted;
			auto& entry = q.sends[(q.first + q.submitted - 1) % q.sends.size()];
			Buffer::release(entry.payload);
			entry.payload = nullptr;
			if(q.submitted == 1){ retire(q, 1, retired); }
			continue;
		}
		// callbacks may run synchronously here and only signal native work
		bool direct = false, cancelled = false;
		try{
			direct = transport->send(reinterpret_cast<const std::byte*>(payload->data()), payload->size, credit);
		} catch(const std::runtime_error& error){
			// the pinned backend rejects a closed channel before SCTP admission
			if(!transport->isClosed() || std::string_view(error.what()) != "DataChannel is closed"){ throw; }
			cancelled = true;
		}
		if(!direct && backend->state() != ::rtc::PeerConnection::State::Connected){
			throw Error(11, "Transport failed during backend admission");
		}
		const bool canWait = !cancelled && !channel->closed && transport->isOpen();
		{
			std::lock_guard lock(q.mutex);
			auto& entry = q.sends[(q.first + q.submitted) % q.sends.size()];
			++q.submitted;
			Buffer::release(entry.payload);
			entry.payload = nullptr;
			if(!cancelled){
				if(entry.size > (std::numeric_limits<size_t>::max)() - q.handedBytes){
					q.handedOverflow = true;
				} else{
					q.handedBytes += entry.size;
				}
			}
			if(direct || (cancelled && q.submitted == 1)){
				retire(q, q.submitted, retired);
			} else if(entry.size != 0 && canWait && !q.handedOverflow){
				q.waiting = true;
			}
		}
	}
	schedule(shared_from_this());
}

void Peer::purge_sends() noexcept {
	if(!queues){ return; }
	Retired retired;
	std::lock_guard lock(queues->mutex);
	// operations excludes the dispatcher and all in-flight backend calls
	queues->submitted = queues->sendMessages;
	retire(*queues, queues->submitted, retired);
	queues->waiting = false;
}
}
