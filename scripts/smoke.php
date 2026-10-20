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

require_once __DIR__ . '/test-process.php';

$module = isset($argv[1]) ? realpath($argv[1]) : false;
if($module === false || !is_file($module)){
	fwrite(STDERR, "Usage: php -n scripts/smoke.php ABSOLUTE_MODULE_PATH\n");
	exit(1);
}
$worker = <<<'PHP'
if(!extension_loaded('rtc') || phpversion('rtc') !== '0.1.0-dev'){ exit(2); }
$c = \amber\rtc\RtcRuntime::getCapabilities();
if($c !== ['nativeBootstrap'=>true,'peerConnection'=>true,'dataChannels'=>false,'trickleIce'=>true,'observedRemoteFingerprint'=>true,'media'=>false]){ exit(3); }
if(\amber\rtc\RtcRuntime::getStats() !== ['activePeers'=>0,'activeChannels'=>0,'managedBufferBytes'=>0,'receiveOverflowCount'=>0,'sendRejectedCount'=>0]){ exit(4); }
if(\amber\rtc\RtcRuntime::getApiVersion() !== 1 || !class_exists('amber\\rtc\\PeerConnection', false)){ exit(5); }
$peer = new \amber\rtc\PeerConnection(\amber\rtc\PeerConnectionOptions::create());
if(\amber\rtc\RtcRuntime::getStats()['activePeers'] !== 1){ exit(6); }
$peer->close(); unset($peer);
$unclosed = new \amber\rtc\PeerConnection(\amber\rtc\PeerConnectionOptions::create());
echo "OK\n";
PHP;
for($i = 0; $i < 100; ++$i){
	$pipes = [];
	[$process, $pipes] = startTestProcess([PHP_BINARY, '-n', '-d', 'extension=' . $module, '-r', $worker]);
	if(!is_resource($process)){ throw new RuntimeException('Unable to start smoke child'); }
	fclose($pipes[0]);
	$out = $err = '';
	$deadline = hrtime(true) + 10_000_000_000;
	do{
		$out .= processOutput($pipes[1]);
		$err .= processOutput($pipes[2]);
		$status = proc_get_status($process);
		if(!$status['running']){ break; }
		if(hrtime(true) >= $deadline){
			proc_terminate($process, 9);
			fclose($pipes[1]);
			fclose($pipes[2]);
			proc_close($process);
			throw new RuntimeException("Smoke child $i timed out");
		}
		usleep(10000);
	}while(true);
	$out .= processOutput($pipes[1]);
	$err .= processOutput($pipes[2]);
	fclose($pipes[1]);
	fclose($pipes[2]);
	$closedCode = proc_close($process);
	$code = $status['exitcode'] >= 0 ? $status['exitcode'] : $closedCode;
	if($code !== 0 || $out !== "OK\n" || $err !== ''){
		throw new RuntimeException("Smoke child $i failed, exit=$code, stdout=" . $out . ', stderr=' . $err);
	}
}
echo "PASS: 100/100 real module load/shutdown processes\n";
