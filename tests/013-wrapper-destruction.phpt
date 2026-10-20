--TEST--
rtc wrapper destruction closes transports during gathering and leaves no active peers
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
use amber\rtc\{PeerConnection as Peer, PeerConnectionOptions as Options, RtcRuntime};
[$proc,$pipes,$offer]=offerer();
for($i=0;$i<32;++$i){
	$p=new Peer(Options::create());
	$p->setRemoteOffer($offer);
	if($i%2===0){ $p->close(); }
	unset($p);
	check(RtcRuntime::getStats()['activePeers']===0,'wrapper destruction closes immediately');
}
stopOfferer($proc,$pipes);
echo "wrapper destruction OK\n";
?>
--EXPECT--
wrapper destruction OK
