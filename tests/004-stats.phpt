--TEST--
rtc bootstrap statistics have no transport allocations
--FILE--
<?php
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

declare(strict_types=1);
$want = ['activePeers'=>0, 'activeChannels'=>0, 'managedBufferBytes'=>0, 'receiveOverflowCount'=>0, 'sendRejectedCount'=>0];
var_dump(amber\rtc\RtcRuntime::getStats() === $want);
$s = amber\rtc\RtcRuntime::getStats();
$s['activePeers'] = 100;
var_dump(amber\rtc\RtcRuntime::getStats() === $want);
?>
--EXPECT--
bool(true)
bool(true)
