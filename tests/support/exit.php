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

use amber\rtc\{GatheringState, PeerConnection as Peer, PeerConnectionOptions as Options, RtcException, RtcRuntime};

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
	case 'mux':
		// exit while two answerers share a live mux socket and its backend thread
		$probe = stream_socket_server('udp://127.0.0.1:0', $code, $error, STREAM_SERVER_BIND);
		$port = (int) substr(strrchr(stream_socket_get_name($probe, false), ':'), 1);
		fclose($probe);
		$channel = $peer->createDataChannel('exit');
		$deadline = hrtime(true) + 10_000_000_000;
		while($peer->getGatheringState() !== GatheringState::COMPLETE){
			if(hrtime(true) > $deadline){ exit(4); }
			usleep(1000);
		}
		$muxed = [];
		for($i = 0; $i < 2; ++$i){
			$muxed[$i] = new Peer(Options::create()->setBindAddress('127.0.0.1')->setPortRange($port, $port)->setIceUdpMuxEnabled(true));
			$muxed[$i]->setRemoteOffer($peer->getLocalDescription());
		}
		break;
	default:
		exit(2);
}
if(!in_array(RtcRuntime::getStats()['activePeers'], $mode === 'explicit' || $mode === 'unset' || $mode === 'rejected' ? [0] : ($mode === 'mux' ? [3] : [1]), true)){ exit(3); }
echo "DONE\n";
