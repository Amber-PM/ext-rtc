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

#include <atomic>
#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string_view>

namespace amber::rtc {

constexpr size_t bufferLimit = 67108864;
// queued sends share this part of the budget, receives and new peers keep the rest
constexpr size_t sendGeneral = bufferLimit / 4 * 3;
// small first messages of late peers may also use this extra part
constexpr size_t sendBudget = bufferLimit / 8 * 7;
constexpr size_t sendFloor = 65536;
// the sndbuf of one SCTP association is 1 MiB, a peer that had more than this many bytes accepted has seen real ACKs
constexpr size_t provenBytes = 4 * 1048576;
// peers that did not prove it leave this part of the general pool to the ones that did, frozen newcomers cannot take it
constexpr size_t provenHeadroom = sendGeneral / 4;
// a queue is limited as if at least this many peers were connected so early peers cannot take it all
constexpr size_t sharePeers = 64;
size_t managed_buffer_bytes();
size_t budget_bytes();
size_t send_cost(size_t payload);
// proven peers may also use the last part of the general pool, see provenHeadroom
bool reserve_send(size_t own, size_t cost, bool proven = false);
void release_send(size_t cost) noexcept;
size_t send_footprint();

size_t receive_overflow_count();
size_t send_rejected_count();
void count_overflow();
void count_rejection();
void saturating_increment(std::atomic<size_t>& value);
void reserve_escrow(size_t bytes);
void release_escrow(size_t bytes) noexcept;

class ManagedCharge{
	size_t bytes = 0;
public:
	ManagedCharge() = default;
	explicit ManagedCharge(size_t amount){ resize(amount); }
	ManagedCharge(const ManagedCharge&) = delete;
	ManagedCharge& operator=(const ManagedCharge&) = delete;
	~ManagedCharge(){ resize(0); }
	void resize(size_t amount);
	void swap(ManagedCharge& other) noexcept { std::swap(bytes, other.bytes); }
};

// one allocation holds the entry and its payload, including an empty-message byte
struct Buffer{
	Buffer* next = nullptr;
	size_t size = 0;
	size_t charge = 0;
	char* data(){ return reinterpret_cast<char*>(this + 1); }
	static Buffer* copy(std::string_view payload);
	static void release(Buffer* buffer) noexcept;
};

struct Escrow{
	ManagedCharge allocation;
	size_t bytes = 0;
	~Escrow(){ release_escrow(bytes); }
};
std::shared_ptr<Escrow> make_escrow(size_t bytes);

struct Channel;
struct SendEntry{
	std::shared_ptr<Channel> channel;
	Buffer* payload = nullptr;
	size_t size = 0, escrow = 0, reserved = 0;
	std::shared_ptr<Escrow> credit;
};

struct Queues{
	ManagedCharge allocation;
	std::mutex mutex;
	size_t receiveBytes = 0, receiveMessages = 0;
	size_t sendBytes = 0, sendMessages = 0, sendFootprint = 0;
	std::atomic<size_t> overflows{0}, rejected{0};
	std::array<SendEntry, 256> sends;
	size_t first = 0, submitted = 0;
	bool waiting = false;
	size_t handedBytes = 0;
	bool handedOverflow = false;
};
std::shared_ptr<Queues> make_queues();
}
