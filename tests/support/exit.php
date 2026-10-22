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

use amber\rtc\{PeerConnection as Peer, PeerConnectionOptions as Options, RtcException, RtcRuntime};

// the process ends right after the backend starts async certificate work
$mode = $argv[1] ?? '';
$peer = new Peer(Options::create());
switch($mode){
	case 'explicit':
		$peer->close();
		break;
	case 'implicit':
		break;
	case 'unset':
		unset($peer);
		break;
	case 'channel':
		$channel = $peer->createDataChannel('exit');
		break;
	case 'rejected':
		try{ $peer->setRemoteOffer('invalid'); }catch(RtcException){}
		$peer->close();
		break;
	case 'exit':
		echo "DONE\n";
		exit(0);
	default:
		exit(2);
}
if(!in_array(RtcRuntime::getStats()['activePeers'], $mode === 'explicit' || $mode === 'unset' || $mode === 'rejected' ? [0] : [1], true)){ exit(3); }
echo "DONE\n";
