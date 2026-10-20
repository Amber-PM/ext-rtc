--TEST--
rtc snapshots, lifecycle and state errors
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
use amber\rtc\{PeerConnection as Peer, PeerConnectionOptions as Options, IceServer, IceCandidate, RtcRuntime, RtcException, ConnectionState, GatheringState};
function rejects(callable $f, int $code) : void{
	try{ $f(); throw new RuntimeException('unexpected success'); }
	catch(RtcException $e){ if($e->getCode() !== $code){ throw $e; } }
}
$o = Options::create();
$p = new Peer($o);
if($p->getState() !== ConnectionState::NEW || $p->getGatheringState() !== GatheringState::NEW || $p->getFailureState() !== null || $p->getRemoteFingerprint() !== null || $p->getLocalDescription() !== null){ throw new RuntimeException('initial state'); }
if(RtcRuntime::getStats()['activePeers'] !== 1){ throw new RuntimeException('active peers'); }
$o->setMaxRemoteDescriptionSize(1);
rejects(fn() => $p->setRemoteOffer(''),4);
rejects(fn() => $p->setRemoteOffer('invalid'),4);
rejects(fn() => $p->setRemoteOffer(str_repeat('x',65537)),5);
rejects(fn() => $p->setRemoteAnswer('invalid'),3);
rejects(fn() => new Peer(Options::create()->setMaxReceiveQueueSize(1)),1);
rejects(fn() => new Peer(Options::create()->setIceTcpEnabled(true)),1);
rejects(fn() => new Peer(Options::create()->setIceUdpMuxEnabled(true)),1);
rejects(fn() => new Peer(Options::create()->setBindAddress('192.0.2.1')),1);
rejects(fn() => new Peer(Options::create()->setIceServers(IceServer::turn('localhost',3478,'u','p','tcp'))),1);
$p->close(); $p->close();
if($p->getState() !== ConnectionState::CLOSED || $p->pollLocalCandidates() !== [] || RtcRuntime::getStats()['activePeers'] !== 0){ throw new RuntimeException('closed state'); }
rejects(fn() => $p->setRemoteOffer(''),2);
rejects(fn() => $p->addRemoteCandidate(new IceCandidate('candidate:1 1 UDP 1 127.0.0.1 5000 typ host','0')),2);
for($i=0;$i<30;++$i){ $peer = new Peer(Options::create()); if($i%2===0){ $peer->close(); } unset($peer); }
if(RtcRuntime::getStats()['activePeers'] !== 0){ throw new RuntimeException('destructor cleanup'); }
echo "lifecycle OK\n";
?>
--EXPECT--
lifecycle OK
