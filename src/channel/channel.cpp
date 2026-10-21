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
#include <algorithm>

namespace amber::rtc {
namespace {
std::atomic<size_t> active{0};
}

void validate_channel_text(std::string_view text){
	if(text.size() > 65535){
		throw Error(1, "Channel text exceeds limit");
	}
	for(size_t i = 0; i < text.size();){
		auto c = static_cast<unsigned char>(text[i++]);
		if(c == 0){
			throw Error(1, "Channel text contains NUL");
		}
		if(c < 0x80){
			continue;
		}
		unsigned count;
		uint32_t value, minimum;
		if(c >= 0xc2 && c <= 0xdf){
			count = 1; value = c & 0x1f; minimum = 0x80;
		} else if(c >= 0xe0 && c <= 0xef){
			count = 2; value = c & 0x0f; minimum = 0x800;
		} else if(c >= 0xf0 && c <= 0xf4){
			count = 3; value = c & 7; minimum = 0x10000;
		} else{
			throw Error(1, "Invalid channel UTF-8");
		}
		if(count > text.size() - i){
			throw Error(1, "Truncated channel UTF-8");
		}
		while(count--){
			auto next = static_cast<unsigned char>(text[i++]);
			if((next & 0xc0) != 0x80){
				throw Error(1, "Invalid channel UTF-8");
			}
			value = (value << 6) | (next & 0x3f);
		}
		if(value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)){
			throw Error(1, "Invalid channel UTF-8");
		}
	}
}

void validate_channel_options(const ChannelOptions& options){
	const auto& r = options.reliability;
	if((r.maxRetransmits && r.maxPacketLifeTime) ||
		(r.maxRetransmits && *r.maxRetransmits > 65535) ||
		(r.maxPacketLifeTime && (r.maxPacketLifeTime->count() < 0 || r.maxPacketLifeTime->count() > 65535))){
		throw Error(1, "Invalid channel reliability");
	}
	validate_channel_text(options.protocol);
}

std::shared_ptr<Channel> Channel::attach(const std::shared_ptr<Peer>& peer,
	const std::shared_ptr<::rtc::DataChannel>& transport, bool incoming){
	ManagedCharge allocation(sizeof(Channel) + 64 + 131072);
	auto self = std::make_shared<Channel>();
	self->allocation.swap(allocation);
	std::lock_guard installation(self->lifecycle);
	self->parent = peer;
	self->queues = peer->queues;
	self->backend = transport;
	self->label = transport->label();
	self->settings.protocol = transport->protocol();
	self->settings.reliability = transport->reliability();
	validate_channel_text(self->label);
	validate_channel_options(self->settings);
	self->allocation.resize(sizeof(Channel) + 64 + self->label.capacity() + self->settings.protocol.capacity() + 2);
	self->maximum = (std::min)(peer->options.message, transport->maxMessageSize());
	self->configuredMaximum = peer->options.message;
	std::shared_ptr<Channel> replaced;
	{
		std::lock_guard lock(peer->mutex);
		if(peer->closed){
			throw Error(2, "Peer is closed");
		}
		auto slot = std::find_if(peer->channels.begin(), peer->channels.end(), [](const auto& c){
			return !c || c->is_closed();
		});
		if(slot == peer->channels.end() || (incoming && peer->pendingChannels.size() >= peer->options.pending)){
			throw Error(8, "Channel limit exceeded");
		}
		if(incoming){
			peer->pendingChannels.push_back(self);
		}
		replaced = std::move(*slot);
		*slot = self;
		self->counted = true;
		++active;
	}
	std::weak_ptr<Channel> weak = self;
	try{
		transport->onOpen([weak]() noexcept {
			if(auto c = weak.lock()){
				if(!c->closed){
					c->opened = true;
				}
			}
		});
		transport->onClosed([weak]() noexcept {
			if(auto c = weak.lock()){
				c->observe_closed();
				c->closeRequested = true;
				if(auto p = c->parent.lock()){ schedule(p); }
			}
		});
		transport->onError([weak](auto) noexcept {
			if(auto c = weak.lock()){
				c->observe_closed();
				c->closeRequested = true;
				if(auto p = c->parent.lock()){ schedule(p); }
			}
		});
		transport->onMessage([weak](::rtc::message_variant payload) noexcept {
			if(auto c = weak.lock()){
				std::visit([&](const auto& bytes){
					c->incoming(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
				}, payload);
			}
		});
		transport->onBufferedAmountLow([weak]() noexcept {
			if(auto c = weak.lock()){
				if(auto p = c->parent.lock()){ schedule(p); }
			}
		});
		self->opened = transport->isOpen();
		if(transport->isClosed()){
			self->observe_closed();
		}
	} catch(...){
		self->observe_closed();
		throw;
	}
	return self;
}

Channel::~Channel(){
	close_safely();
}

void Channel::observe_closed() noexcept {
	closed = true;
	opened = false;
	purge_receive();
	if(counted.exchange(false)){
		--active;
	}
}

void Channel::close(){
	std::lock_guard installation(lifecycle);
	closeRequested = false;
	observe_closed();
	auto transport = std::move(backend);
	if(transport){
		maximum = (std::min)(configuredMaximum, transport->maxMessageSize());
		transport->resetCallbacks();
		transport->close();
		if(auto p = parent.lock()){ schedule(p); }
	}
}

void Channel::close_safely() noexcept {
	try{
		close();
	} catch(...){
		if(auto p = parent.lock()){
			std::lock_guard lock(p->mutex);
			if(!p->closed){
				p->failure = ::rtc::PeerConnection::State::Failed;
				p->state = *p->failure;
			}
		}
	}
}

bool Channel::is_open() const {
	return opened && !closed;
}

bool Channel::is_closed() const {
	return closed;
}

size_t Channel::capacity(){
	std::lock_guard installation(lifecycle);
	if(backend && !closed){
		maximum = (std::min)(configuredMaximum, backend->maxMessageSize());
	}
	return maximum;
}

size_t active_channels(){
	return active.load();
}

std::shared_ptr<Channel> Peer::create_channel(const std::string& label, const ChannelOptions& settings){
	validate_channel_text(label);
	validate_channel_options(settings);
	std::lock_guard operation(operations);
	require_open();
	{
		std::lock_guard lock(mutex);
		if(std::none_of(channels.begin(), channels.end(), [](const auto& c){ return !c || c->is_closed(); })){
			throw Error(8, "Channel limit exceeded");
		}
	}
	::rtc::DataChannelInit init;
	init.protocol = settings.protocol;
	init.reliability = settings.reliability;
	auto transport = backend->createDataChannel(label, init);
	try{
		auto channel = Channel::attach(shared_from_this(), transport, false);
		if(!backend->localDescription()){
			backend->setLocalDescription(::rtc::Description::Type::Offer);
		}
		return channel;
	} catch(...){
		transport->close();
		throw;
	}
}

std::vector<std::shared_ptr<Channel>> Peer::poll_channels(){
	std::vector<std::shared_ptr<Channel>> items;
	std::lock_guard lock(mutex);
	if(!closed){
		items.swap(pendingChannels);
	}
	return items;
}
}
