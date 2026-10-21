--TEST--
rtc receive byte and message limits aggregate across live channels
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
use amber\rtc\{PeerConnection as P,PeerConnectionOptions as O,DataChannelOptions as D,GatheringState as G};
foreach([false,true] as $unreliable){
	$o=O::create()->setMaxMessageSize(4)->setMaxReceiveQueueSize(4)->setMaxReceiveQueueMessages(2);
	$a=new P($o);$b=new P($o);$options=D::create();if($unreliable){$options->setUnordered(true)->setMaxRetransmits(0);}
	$c=$a->createDataChannel('first',$options);
	waitFor(fn()=> $a->getGatheringState()===G::COMPLETE);$b->setRemoteOffer($a->getLocalDescription());
	waitFor(fn()=> $b->getGatheringState()===G::COMPLETE);$a->setRemoteAnswer($b->getLocalDescription());
	$remote=[];waitFor(function()use($b,&$remote){$remote=array_merge($remote,$b->pollDataChannels());return count($remote)===1;});
	$first=$remote[0];$d=$a->createDataChannel('second',$options);
	$remote=[];waitFor(function()use($b,&$remote){$remote=array_merge($remote,$b->pollDataChannels());return count($remote)===1;});$second=$remote[0];
	waitFor(fn()=> $c->isOpen() && $d->isOpen() && $first->isOpen() && $second->isOpen());
	check($c->send('ab') && $d->send('cd'),'aggregate admission');
	waitFor(fn()=> $first->getQueuedMessageCount()===1 && $second->getQueuedMessageCount()===1);
	check($b->getStats()['receiveQueueBytes']===4 && $b->getStats()['receiveQueueMessages']===2,'aggregate RX counters');
	check($c->send(''),'empty overflow send');waitFor(fn()=> $b->getStats()['receiveOverflowCount']===1);
	if($unreliable){check($first->receive()==='ab' && $second->receive()==='cd' && $first->isOpen(),'unreliable aggregate drop-new');}
	else{rtcError(fn()=> $first->receive(),9);check($second->receive()==='cd','unaffected reliable stream lost');}
	$a->close();$b->close();
}
echo "aggregate receive limits OK\n";
?>
--EXPECT--
aggregate receive limits OK
