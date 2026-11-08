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

#include <memory>
#include <chrono>

namespace amber::rtc {
struct Peer;
void start_dispatcher();
void stop_dispatcher() noexcept;
void schedule(const std::shared_ptr<Peer>& peer) noexcept;
void schedule_after(const std::shared_ptr<Peer>& peer, std::chrono::milliseconds delay) noexcept;
// a worker inside a backend call longer than the stall limit stops counting against the concurrent sender limit
// thread count and workers currently counted as stuck, for the tests
size_t dispatcher_workers() noexcept;
size_t dispatcher_stalled() noexcept;
void backend_call_begin() noexcept;
void backend_call_end() noexcept;
}
