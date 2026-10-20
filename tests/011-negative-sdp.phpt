--TEST--
rtc rejects structural SDP errors and preserves limits before backend mutation
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
use amber\rtc\{PeerConnection as Peer, PeerConnectionOptions as Options, ConnectionState as State};
[$proc,$pipes,$offer]=offerer();
$p=new Peer(Options::create());
foreach([
	str_replace('v=0','v=0junk',$offer),
	preg_replace('/^o=.*\r?\n/m',"o=broken\r\n",$offer),
	preg_replace('/^t=.*\r?\n/m',"t=invalid\r\n",$offer),
	preg_replace('/^a=ice-ufrag:.*\r?\n/m','',$offer),
	$offer."a=fingerprint:sha-384 AA:AA\r\n",
	$offer."a=candidate:bad\r\n",
	$offer."m=audio 9 RTP/AVP 0\r\n",
] as $sdp){ rtcError(fn()=>$p->setRemoteOffer($sdp),4); }
check($p->getState()===State::NEW && $p->getLocalDescription()===null,'invalid SDP does not mutate peer');
$small=new Peer(Options::create()->setMaxRemoteDescriptionSize(strlen($offer)-1));
rtcError(fn()=>$small->setRemoteOffer($offer),5);
$small->close();
$p->close(); stopOfferer($proc,$pipes);
echo "SDP admission OK\n";
?>
--EXPECT--
SDP admission OK
