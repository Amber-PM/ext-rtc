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

#pragma once

#include "../peer/peer.h"
#include <rtc/datachannel.hpp>
#include <string_view>

namespace amber::rtc {

struct ChannelOptions{
	::rtc::Reliability reliability;
	std::string protocol;
};

void validate_channel_text(std::string_view text);
void validate_channel_options(const ChannelOptions& options);
size_t active_channels();

struct Channel : std::enable_shared_from_this<Channel>{
	ManagedCharge allocation;
	static std::shared_ptr<Channel> attach(const std::shared_ptr<Peer>& peer,
		const std::shared_ptr<::rtc::DataChannel>& backend, bool incoming);
	~Channel();
	void close();
	void close_safely() noexcept;
	bool is_open() const;
	bool is_closed() const;
	size_t capacity();
	void observe_closed() noexcept;
	void incoming(std::string_view payload) noexcept;
	void purge_receive() noexcept;
	std::optional<size_t> read_size();
	bool copy_front(char* target, size_t size, bool consume);
	size_t available_bytes();
	size_t queued_messages();
	bool send(std::string_view payload);
	size_t buffered_bytes();
	std::shared_ptr<Queues> queues;
	Buffer* receiveHead = nullptr;
	Buffer* receiveTail = nullptr;
	size_t receiveBytes = 0, receiveMessages = 0;
	size_t sendBytes = 0;
	int receiveError = 0;
	std::atomic<bool> closeRequested{false};
	std::weak_ptr<Peer> parent;
	std::shared_ptr<::rtc::DataChannel> backend;
	std::string label;
	ChannelOptions settings;
	// installation and teardown never run under the peer registry lock
	std::mutex lifecycle;
	std::atomic<bool> closed{false}, opened{false}, counted{false};
	std::atomic<size_t> maximum{0};
	size_t configuredMaximum = 0;
};
}
