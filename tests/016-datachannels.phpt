--TEST--
rtc native DataChannel binary interoperability and reliability metadata
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
use amber\rtc\{PeerConnection, PeerConnectionOptions, GatheringState, RtcRuntime};
[$process,$pipes,$offer]=offerer(true);
$p=new PeerConnection(PeerConnectionOptions::create()->setMaxPendingDataChannels(4));
$p->setRemoteOffer($offer);
waitFor(fn()=> $p->getGatheringState()===GatheringState::COMPLETE);
fwrite($pipes[0],bin2hex($p->getLocalDescription())."\n");
check(readOffererLine($pipes[1])==='STATE connected','native handshake');
$channels=[];
waitFor(function()use($p,&$channels){foreach($p->pollDataChannels() as $c){$channels[$c->getLabel()]=$c;}return count($channels)===3;});
check($p->pollDataChannels()===[],'duplicate channel handoff');
$r=$channels['transport-test'];$u=$channels['unreliable-test'];$t=$channels['timed-test'];
waitFor(fn()=> $r->isOpen() && $u->isOpen() && $t->isOpen());
check(!$r->isUnordered() && $r->getMaxRetransmits()===null && $r->getMaxPacketLifeTime()===null,'reliable metadata');
check($u->isUnordered() && $u->getMaxRetransmits()===0 && $u->getProtocol()==='binary','unreliable metadata');
check($t->getMaxRetransmits()===null && $t->getMaxPacketLifeTime()===25,'lifetime metadata');
function command(array $pipes,string $line): string{fwrite($pipes[0],$line."\n");return readOffererLine($pipes[1]);}
foreach([['transport-test',0],['unreliable-test',1],['timed-test',2]] as [$name,$index]){
	$c=$channels[$name];
	foreach(["a\0\xffb",'',str_repeat('z',262144)] as $payload){
		check($c->send($payload),'send admission');
		waitFor(function()use($pipes,$index,$payload){$line=command($pipes,'RECEIVE '.$index);if($line==='EMPTY'){return false;}check($line===rtrim('DATA '.bin2hex($payload)),'native receive binary');return true;});
		check(command($pipes,'SEND '.$index.' '.bin2hex($payload))==='SENT','native send');
		waitFor(fn()=> $c->getQueuedMessageCount()===1);
		check($c->getAvailableAmount()===strlen($payload),'available bytes');
		check($c->peek()===$payload && $c->peek()===$payload && $c->receive()===$payload && $c->receive()===null,'peek receive FIFO');
	}
	rtcError(fn()=> $c->send(str_repeat('x',262145)),6);
}
for($i=0;$i<50;++$i){check($r->send(pack('N',$i)),'ordered admission');}
for($i=0;$i<50;++$i){waitFor(function()use($pipes,$i){$line=command($pipes,'RECEIVE 0');if($line==='EMPTY'){return false;}check($line==='DATA '.bin2hex(pack('N',$i)),'reliable FIFO or duplicate');return true;});}
check(command($pipes,'RECEIVE 0')==='EMPTY','duplicate messages');
check(command($pipes,'CLOSECHANNEL 1')==='CLOSED','remote close');waitFor(fn()=> $u->isClosed());
$stats=$p->getStats();check($stats['bytesSent']>0 && $stats['bytesReceived']>0 && $stats['receiveQueueMessages']===0,'payload stats');
$max=$r->getMaxMessageSize();$p->close();unset($p);
check($r->isClosed() && !$r->isOpen() && $r->getLabel()==='transport-test' && $r->getMaxMessageSize()===$max,'retained metadata');
check(RtcRuntime::getStats()['activeChannels']===0,'active channels after close');
stopOfferer($process,$pipes);
echo "native binary channels OK\n";
?>
--EXPECT--
native binary channels OK
