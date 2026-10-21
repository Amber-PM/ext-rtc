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
require __DIR__.'/network.php';
use amber\rtc\{PeerConnection as P,PeerConnectionOptions as O,GatheringState as G,RtcRuntime};
$mode=$argv[1];
register_shutdown_function(static function():void{
	$GLOBALS['bulk']=null;$GLOBALS['chunks']=null;
	if(isset($GLOBALS['a'])){$GLOBALS['a']->close();}
	if(isset($GLOBALS['b'])){$GLOBALS['b']->close();}
	check($GLOBALS['b']->getStats()['receiveQueueBytes']===0 && $GLOBALS['a']->getStats()['sendQueueBytes']===0,'bailout queue purge');
	unset($GLOBALS['a'],$GLOBALS['b'],$GLOBALS['channels'],$GLOBALS['incoming'],$GLOBALS['remote']);
	// the failing vm call retains its channel until request deactivation
	waitFor(fn()=> RtcRuntime::getStats()['activePeers']===0 && RtcRuntime::getStats()['activeChannels']===0 && RtcRuntime::getStats()['managedBufferBytes']<=$GLOBALS['baseline']);
	echo "CLEAN\n";
});
$o=O::create()->setMaxMessageSize(2097152)->setMaxSendQueueSize(4194304);
$a=new P($o);$b=new P($o);$channels=[];
for($i=0;$i<1;++$i){$channels[]=$a->createDataChannel('bailout-'.$i);}
waitFor(fn()=> $a->getGatheringState()===G::COMPLETE);$b->setRemoteOffer($a->getLocalDescription());
waitFor(fn()=> $b->getGatheringState()===G::COMPLETE);$a->setRemoteAnswer($b->getLocalDescription());
waitFor(fn()=> RtcRuntime::getStats()['activeChannels']===2);
{
	$incoming=[];waitFor(function()use($b,&$incoming){$incoming=array_merge($incoming,$b->pollDataChannels());return count($incoming)===1;});
	$remote=$incoming[0];waitFor(fn()=> $channels[0]->isOpen() && $remote->isOpen());
	$baseline=RtcRuntime::getStats()['managedBufferBytes'];
	check($channels[0]->send(str_repeat('x',2097152)),'bailout payload admission');waitFor(fn()=> $remote->getQueuedMessageCount()===1);
}
$bulk=str_repeat('x',16*1024*1024-memory_get_usage(true)-65536);
echo "TRIGGER\n";
$remote->$mode();
throw new RuntimeException('expected Zend allocation bailout');
