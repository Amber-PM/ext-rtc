--TEST--
rtc local channels, aggregate receive limits and request-owned close
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
require __DIR__.'/support/network.php';
use amber\rtc\{PeerConnection as P,PeerConnectionOptions as O,DataChannelOptions as D,GatheringState as G,RtcRuntime};
function pair(bool $unreliable):array{
	$o=O::create()->setMaxMessageSize(4)->setMaxReceiveQueueSize(4)->setMaxReceiveQueueMessages(2);
	$a=new P($o);$b=new P($o);
	$d=D::create();if($unreliable){$d->setUnordered(true)->setMaxRetransmits(0);}
	$c=$a->createDataChannel('generic',$d);
	waitFor(fn()=> $a->getGatheringState()===G::COMPLETE);$b->setRemoteOffer($a->getLocalDescription());
	waitFor(fn()=> $b->getGatheringState()===G::COMPLETE);$a->setRemoteAnswer($b->getLocalDescription());
	$remote=[];waitFor(function()use($b,&$remote){$remote=array_merge($remote,$b->pollDataChannels());return count($remote)===1;});
	waitFor(fn()=> $c->isOpen() && $remote[0]->isOpen());return [$a,$b,$c,$remote[0]];
}
foreach([false,true] as $unreliable){
	[$a,$b,$c,$r]=pair($unreliable);
	check($a->pollDataChannels()===[],'local duplicate');
	check($c->getMaxMessageSize()===4,'public capacity');
	check($c->send('head'),'head admission');waitFor(fn()=> $r->getQueuedMessageCount()===1);
	check($c->send('x'),'overflow admission');waitFor(fn()=> $b->getStats()['receiveOverflowCount']===1);
	if($unreliable){check($r->peek()==='head' && $r->receive()==='head' && $r->isOpen(),'unreliable drop-new');}
	else{rtcError(fn()=> $r->peek(),9);rtcError(fn()=> $r->receive(),9);$r->close();rtcError(fn()=> $r->receive(),9);}
	$a->close();$b->close();check($b->getStats()['receiveQueueBytes']===0 && $b->getStats()['receiveQueueMessages']===0,'RX purge');
}
[$a,$b,$c,$r]=pair(false);
check($c->send(''),'first empty');waitFor(fn()=> $r->getQueuedMessageCount()===1);
check($c->send(''),'second empty');waitFor(fn()=> $r->getQueuedMessageCount()===2);
check($r->getAvailableAmount()===0 && $r->peek()==='' && $r->receive()==='' && $r->receive()==='' && $r->receive()===null,'empty slots');
check($c->send(''),'empty flood 1');waitFor(fn()=> $r->getQueuedMessageCount()===1);
check($c->send(''),'empty flood 2');waitFor(fn()=> $r->getQueuedMessageCount()===2);
check($c->send(''),'empty flood 3');waitFor(fn()=> $b->getStats()['receiveOverflowCount']===1);
rtcError(fn()=> $r->peek(),9);
unset($a);check($c->isClosed(),'parent destroyed first');rtcError(fn()=> $c->send(''),2);$b->close();
check(RtcRuntime::getStats()['activePeers']===0 && RtcRuntime::getStats()['activeChannels']===0,'transport cleanup');
echo "local channels and receive limits OK\n";
?>
--EXPECT--
local channels and receive limits OK
