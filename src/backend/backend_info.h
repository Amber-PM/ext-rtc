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

namespace amber::rtc {
inline constexpr auto expected_backend_identity = "libdatachannel/0.24.6;commit=6b1e2e620f1e37f0eafeee702eaea0043cb305fd;message-lifetime=82a2c4c4e98eb5b5aa779edccaa7933ea15ad927cf51d410bf5f9f3b4d79891c";
const char* backend_version() noexcept;
const char* backend_identity() noexcept;
bool backend_probe();
}
