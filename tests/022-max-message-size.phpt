--TEST--
rtc advertises the admitted message size while keeping dcep capacity
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
use amber\rtc\{PeerConnection, PeerConnectionOptions as Options, DataChannelOptions, GatheringState, RtcRuntime};
function advertised(PeerConnection $p) : int{
	check(preg_match_all('/^a=max-message-size:(\d+)\r?$/m', (string) $p->getLocalDescription(), $m) === 1, 'one max-message-size');
	return (int) $m[1][0];
}
function deliver($from, $to, string $payload, string $what) : void{
	check($from->send($payload), $what.' admission');
	waitFor(fn()=> $to->peek() !== null);
	check($to->peek() === $payload && $to->receive() === $payload && $to->receive() === null, $what.' delivery');
}
foreach([[1024, 512, str_repeat('L', 1000)], [16, 16, str_repeat('d', 4096)], [262144, 65536, 'ReliableDataChannel']] as [$offerMax, $answerMax, $label]){
	$offer = new PeerConnection(Options::create()->setMaxMessageSize($offerMax));
	$answer = new PeerConnection(Options::create()->setMaxMessageSize($answerMax));
	$reliable = $offer->createDataChannel($label);
	$lossy = $offer->createDataChannel('u', DataChannelOptions::create()->setUnordered(true)->setMaxRetransmits(0));
	waitFor(fn()=> $offer->getGatheringState() === GatheringState::COMPLETE);
	check(advertised($offer) === $offerMax, "offer advertises $offerMax");
	$answer->setRemoteOffer((string) $offer->getLocalDescription());
	waitFor(fn()=> $answer->getGatheringState() === GatheringState::COMPLETE);
	check(advertised($answer) === $answerMax, "answer advertises $answerMax");
	$offer->setRemoteAnswer((string) $answer->getLocalDescription());
	$incoming = [];
	waitFor(function() use ($answer, &$incoming){ foreach($answer->pollDataChannels() as $c){ $incoming[$c->getLabel()] = $c; } return count($incoming) === 2; });
	waitFor(fn()=> $reliable->isOpen() && $lossy->isOpen() && $incoming[$label]->isOpen() && $incoming['u']->isOpen());
	$limit = min($offerMax, $answerMax);
	foreach([[$reliable, $incoming[$label]], [$lossy, $incoming['u']]] as [$local, $remote]){
		check($local->getMaxMessageSize() === $limit && $remote->getMaxMessageSize() === $limit, "negotiated $limit");
		deliver($local, $remote, str_repeat("\x00\xff", intdiv($limit, 2)) . ($limit % 2 ? 'x' : ''), 'exact forward');
		deliver($remote, $local, str_repeat('z', $limit), 'exact reverse');
		deliver($local, $remote, '', 'empty');
		rtcError(fn()=> $local->send(str_repeat('o', $limit + 1)), 6);
		rtcError(fn()=> $remote->send(str_repeat('o', $limit + 1)), 6);
	}
	check($offer->getStats()['receiveOverflowCount'] === 0 && $answer->getStats()['receiveOverflowCount'] === 0, 'no oversize reached a receiver');
	$offer->close();
	$answer->close();
	unset($reliable, $lossy, $incoming, $offer, $answer);
}
check(RtcRuntime::getStats()['activePeers'] === 0 && RtcRuntime::getStats()['activeChannels'] === 0, 'released');
echo "max-message-size OK\n";
?>
--EXPECT--
max-message-size OK
