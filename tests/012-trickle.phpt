--TEST--
rtc negotiates with a remote candidate supplied only through the trickle API
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
use amber\rtc\{PeerConnection as Peer, PeerConnectionOptions as Options, IceCandidate, ConnectionState as State, GatheringState as Gathering};
[$proc,$pipes,$offer]=offerer();
preg_match_all('/^a=(candidate:[^\r\n]+)/m',$offer,$candidates);
check(count($candidates[1])>0,'native candidates');
$withoutCandidates=preg_replace('/^a=candidate:[^\r\n]*\r?\n/m','',$offer);
$p=new Peer(Options::create());
$p->setRemoteOffer($withoutCandidates);
foreach($candidates[1] as $candidate){ $p->addRemoteCandidate(new IceCandidate($candidate,'0')); }
waitFor(fn()=>$p->getGatheringState()===Gathering::COMPLETE);
$answer=preg_replace('/^a=candidate:[^\r\n]*\r?\n/m','',$p->getLocalDescription());
check(!str_contains($answer,'a=candidate:'),'answer has no candidates');
fwrite($pipes[0],bin2hex($answer)."\n");
waitFor(fn()=>$p->getState()===State::CONNECTED);
check(readOffererLine($pipes[1])==='STATE connected','native peer connected');
check($p->getRemoteFingerprint()!==null,'DTLS certificate observed');
$p->close(); stopOfferer($proc,$pipes);
echo "trickle ICE/DTLS connected\n";
?>
--EXPECT--
trickle ICE/DTLS connected
