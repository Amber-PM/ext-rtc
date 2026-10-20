--TEST--
rtc does not register nonfunctional transport APIs
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
$extension = new ReflectionExtension('rtc');
var_dump($extension->getClassNames() === ['amber\rtc\RtcRuntime']);
var_dump($extension->getFunctions() === []);
foreach(['PeerConnection', 'DataChannel', 'PeerConnectionOptions', 'DataChannelOptions', 'IceServer', 'IceCandidate', 'RtcException'] as $name){
	if(class_exists('amber\\rtc\\' . $name, false)){ throw new RuntimeException("unexpected $name"); }
}
foreach(['ConnectionState', 'GatheringState'] as $name){
	if(enum_exists('amber\\rtc\\' . $name, false)){ throw new RuntimeException("unexpected $name"); }
}
try{ new amber\rtc\RtcRuntime(); }catch(Error $e){ echo "construction rejected\n"; }
var_dump($extension->getINIEntries() === []);
?>
--EXPECT--
bool(true)
bool(true)
construction rejected
bool(true)
