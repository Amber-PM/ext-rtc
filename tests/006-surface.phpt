--TEST--
rtc registers transport and native binary data channels
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
$classes = $extension->getClassNames(); sort($classes);
$expected = array_map(fn($n) => 'amber\\rtc\\'.$n, ['RtcRuntime','PeerConnection','PeerConnectionOptions','IceServer','IceCandidate','RtcException','ConnectionState','GatheringState','DataChannel','DataChannelOptions']); sort($expected);
var_dump($classes === $expected);
var_dump($extension->getFunctions() === []);
try{ new amber\rtc\RtcRuntime(); }catch(Error $e){ echo "construction rejected\n"; }
var_dump($extension->getINIEntries() === []);
?>
--EXPECT--
bool(true)
bool(true)
construction rejected
bool(true)
