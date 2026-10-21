--TEST--
rtc channel options preserve zero, reject invalid metadata and copy settings
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
use amber\rtc\{DataChannelOptions as O, PeerConnection, PeerConnectionOptions, RtcException};
function reject(callable $f): void{
	try{ $f(); throw new RuntimeException('invalid option accepted'); }
	catch(RtcException $e){ if($e->getCode()!==1){ throw $e; } }
}
$o=O::create();
if($o->setUnordered(true)!==$o || $o->setMaxRetransmits(0)!==$o){ throw new RuntimeException('fluent options'); }
reject(fn()=> $o->setMaxPacketLifeTime(0));
$o->setMaxRetransmits(null)->setMaxPacketLifeTime(0);
reject(fn()=> $o->setMaxRetransmits(0));
foreach([-1,65536,PHP_INT_MAX] as $n){reject(fn()=> O::create()->setMaxRetransmits($n));reject(fn()=> O::create()->setMaxPacketLifeTime($n));}
foreach(["a\0b","\xc0\x80","\xed\xa0\x80","\xf4\x90\x80\x80",str_repeat('x',65536)] as $text){reject(fn()=>O::create()->setProtocol($text));}
$p=new PeerConnection(PeerConnectionOptions::create());
$c=$p->createDataChannel('arbitrary',$o->setProtocol('binary'));
$o->setProtocol('changed')->setMaxPacketLifeTime(null);
if($c->getProtocol()!=='binary' || $c->getMaxPacketLifeTime()!==0 || !$c->isUnordered() || $c->getMaxRetransmits()!==null || $p->pollDataChannels()!==[]){throw new RuntimeException('channel option copy');}
$c->close();$p->close();
if(!$c->isClosed() || $c->isOpen() || $c->getLabel()!=='arbitrary' || $c->peek()!==null || $c->receive()!==null){throw new RuntimeException('retained closed channel');}
try{$c->send('');throw new RuntimeException('closed send accepted');}catch(RtcException $e){if($e->getCode()!==2){throw $e;}}
echo "channel options OK\n";
?>
--EXPECT--
channel options OK
