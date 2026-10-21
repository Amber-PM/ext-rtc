--TEST--
rtc rejects oversized SCTP input without exposing a truncated reliable prefix
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
use amber\rtc\{PeerConnection,PeerConnectionOptions,GatheringState};
[$process,$pipes,$offer]=offerer();
$p=new PeerConnection(PeerConnectionOptions::create()->setMaxMessageSize(4)->setMaxReceiveQueueSize(4));
$p->setRemoteOffer($offer);waitFor(fn()=> $p->getGatheringState()===GatheringState::COMPLETE);
$answer=preg_replace('/a=max-message-size:\d+/','a=max-message-size:262144',$p->getLocalDescription(),-1,$replacements);
check($replacements===1,'oversize vector negotiation');
fwrite($pipes[0],bin2hex($answer)."\n");check(readOffererLine($pipes[1])==='STATE connected','handshake');
$channels=[];waitFor(function()use($p,&$channels){$channels=array_merge($channels,$p->pollDataChannels());return count($channels)===1;});
$c=$channels[0];waitFor(fn()=> $c->isOpen());
fwrite($pipes[0],'SEND 0 '.bin2hex(str_repeat('x',150000))."\n");check(readOffererLine($pipes[1])==='SENT','oversized native send');
waitFor(fn()=> $p->getStats()['receiveOverflowCount']===1);
rtcError(fn()=> $c->peek(),9);rtcError(fn()=> $c->receive(),9);waitFor(fn()=> $c->isClosed());
check($c->getQueuedMessageCount()===0 && $c->getAvailableAmount()===0,'truncated prefix admitted');
$p->close();stopOfferer($process,$pipes);
echo "oversized SCTP rejected whole\n";
?>
--EXPECT--
oversized SCTP rejected whole
