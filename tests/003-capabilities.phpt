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
	'nativeBootstrap' => true, 'peerConnection' => true, 'dataChannels' => true,
	'trickleIce' => true, 'observedRemoteFingerprint' => true, 'media' => false
]);
$c = amber\rtc\RtcRuntime::getCapabilities();
$c['peerConnection'] = false;
var_dump(amber\rtc\RtcRuntime::getCapabilities()['peerConnection']);
$c['dataChannels'] = false;
var_dump(amber\rtc\RtcRuntime::getCapabilities()['dataChannels']);
$peer = new amber\rtc\PeerConnection(amber\rtc\PeerConnectionOptions::create());
$channel = $peer->createDataChannel('capability');
var_dump($channel instanceof amber\rtc\DataChannel);
$peer->close();
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
