--TEST--
rtc real SDP, ICE, DTLS and forged fingerprint rejection with native offerer
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
use amber\rtc\{PeerConnection as Peer, PeerConnectionOptions as Options, ConnectionState as State, GatheringState as Gathering, IceCandidate};
foreach([false,true] as $forged){
	[$proc,$pipes,$offer]=offerer();
	check(preg_match('/a=fingerprint:sha-256 ([A-F0-9:]{95})/',$offer,$matches)===1,'offer certificate');
	$actual=$matches[1]; $fake=implode(':',array_fill(0,32,'00'));
	$negotiatedOffer=$offer;
	$p=new Peer(Options::create());
	check($p->getRemoteFingerprint()===null,'no certificate before DTLS');
	if(!$forged){
		rtcError(fn()=>$p->setRemoteOffer(str_replace('sha-256','sha-1',$offer)),4);
		$lines=explode("\n",$offer); $base=implode("\n",array_filter($lines,fn($l)=>!str_starts_with($l,'a=candidate:')));
		$c='candidate:1 1 UDP 2122260223 127.0.0.1 5000 typ host';
		rtcError(fn()=>$p->setRemoteOffer($base.str_repeat('a='.$c."\r\n",65)),7);
		$padding='';
		for($i=0;$i<40;++$i){ $padding.='a=candidate:'.($i+100).' 1 UDP 2122260223 127.0.0.1 '.(20000+$i)." typ host\r\n"; }
		$negotiatedOffer=preg_replace('/(?=^a=candidate:)/m',$padding,$offer,1);
	}
	$p->setRemoteOffer($forged?str_replace($actual,$fake,$offer):$negotiatedOffer);
	waitFor(fn()=>$p->getGatheringState()===Gathering::COMPLETE);
	$answer=$p->getLocalDescription(); check(is_string($answer) && str_contains($answer,'a=candidate:'),'full ICE answer');
	check(count($p->pollLocalCandidates())>0 && $p->pollLocalCandidates()===[],'candidate handoff');
	fwrite($pipes[0],bin2hex($answer)."\n");
	waitFor(fn()=>$p->getState()===State::CONNECTED || $p->getFailureState()===State::FAILED);
	if($forged){
		check(in_array($p->getState(),[State::FAILED,State::CLOSED],true),'forged fingerprint must fail');
		check($p->getRemoteFingerprint()===$actual && $p->getRemoteFingerprint()!==$fake,'observed certificate B, never advertised A');
		check($p->getFailureState()===State::FAILED,'failure retained');
	}else{
		check($p->getState()===State::CONNECTED,'real connection');
		check(readOffererLine($pipes[1])==='STATE connected','offerer connected');
		check($p->getRemoteFingerprint()===$actual,'observed SHA-256');
		check($p->getLocalAddress()!==null && $p->getRemoteAddress()!==null,'selected addresses');
		rtcError(fn()=>$p->setRemoteOffer($offer),3);
		preg_match_all('/^a=candidate:/m',$negotiatedOffer,$candidates);
		for($i=count($candidates[0]);$i<64;++$i){ $p->addRemoteCandidate(new IceCandidate($c,'0')); }
		rtcError(fn()=>$p->addRemoteCandidate(new IceCandidate($c,'0')),7);
	}
	$fp=$p->getRemoteFingerprint(); $local=$p->getLocalAddress(); $stats=$p->getStats();
	$p->close(); $p->close();
	check($p->getRemoteFingerprint()===$fp && $p->getLocalAddress()===$local && $p->getLocalDescription()===$answer,'closed snapshots');
	check($p->getStats()===$stats,'final stats');
	if($forged){ check($p->getFailureState()===State::FAILED,'failure after close'); }
	stopOfferer($proc,$pipes);
	echo $forged?"forged certificate rejected, B observed\n":"real ICE/DTLS connected\n";
}
?>
--EXPECT--
real ICE/DTLS connected
forged certificate rejected, B observed
