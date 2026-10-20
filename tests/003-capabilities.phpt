--TEST--
rtc bootstrap advertises only implemented features
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
var_dump(amber\rtc\RtcRuntime::getCapabilities() === [
	'nativeBootstrap' => true, 'peerConnection' => true, 'dataChannels' => false,
	'trickleIce' => true, 'observedRemoteFingerprint' => true, 'media' => false
]);
$c = amber\rtc\RtcRuntime::getCapabilities();
$c['peerConnection'] = false;
var_dump(amber\rtc\RtcRuntime::getCapabilities()['peerConnection']);
?>
--EXPECT--
bool(true)
bool(true)
