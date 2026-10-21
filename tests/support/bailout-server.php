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
use amber\rtc\{PeerConnection as P, PeerConnectionOptions as O, GatheringState as G, RtcRuntime};

$mode=$_GET['mode'] ?? 'stats';
if($mode==='stats'){
	echo json_encode(['pid'=>getmypid(), 'stats'=>RtcRuntime::getStats()]);
	return;
}
if($mode==='memory'){
	ini_set('memory_limit','16M');
	str_repeat('x',32*1024*1024);
	throw new RuntimeException('memory failure missing');
}
$a=new P(O::create());$b=new P(O::create());
$local=$a->createDataChannel('bailout');
waitFor(fn()=> $a->getGatheringState()===G::COMPLETE);
$b->setRemoteOffer($a->getLocalDescription());
waitFor(fn()=> $b->getGatheringState()===G::COMPLETE);
$a->setRemoteAnswer($b->getLocalDescription());
$remote=[];
waitFor(function()use($b,&$remote){$remote=array_merge($remote,$b->pollDataChannels());return count($remote)===1;});
waitFor(fn()=> $local->isOpen() && $remote[0]->isOpen());
$method=$_GET['method'];
rtc_probe_arm($method,(int)($_GET['at'] ?? 0));
$result=$a->$method();
$allocations=rtc_probe_stop();
if($method==='pollLocalCandidates'){
	check(count($result)>0 && $a->pollLocalCandidates()===[],'candidate transfer');
}elseif($method==='getStats'){
	check(count($result)===8 && isset($result['bytesSent']),'stats shape');
}else{
	check(is_string($result) && $result!=='','snapshot result');
}
$a->close();$b->close();
rtcError(fn()=> $a->setRemoteOffer(''),2);
echo json_encode(['pid'=>getmypid(),'allocations'=>$allocations]);
