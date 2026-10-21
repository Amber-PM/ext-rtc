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

require_once __DIR__ . '/../../scripts/test-process.php';

function check(bool $ok, string $message) : void{
	if(!$ok){ throw new RuntimeException($message); }
}

function waitFor(callable $ready, int $seconds = 15) : void{
	$deadline = hrtime(true) + $seconds * 1_000_000_000;
	do{
		if($ready()){ return; }
		usleep(10000);
	}while(hrtime(true) < $deadline);
	throw new RuntimeException('network wait timed out');
}

function readOffererLine($stream) : string{
	$line = '';
	waitFor(function() use($stream, &$line){
		$line .= processOutput($stream);
		return str_ends_with($line, "\n");
	});
	return trim($line);
}

function offerer(bool $data = false) : array{
	$binary = getenv('RTC_TEST_OFFERER');
	check(is_string($binary) && is_file($binary), 'RTC_TEST_OFFERER must name the compiled native harness');
	[$process, $pipes] = startTestProcess($data ? [$binary, '--data'] : [$binary]);
	register_shutdown_function(static function() use($process, $pipes){
		if(!is_resource($process)){ return; }
		if(is_resource($pipes[0])){ fclose($pipes[0]); }
		$deadline = hrtime(true) + 2_000_000_000;
		while(proc_get_status($process)['running']){
			if(hrtime(true) > $deadline){
				proc_terminate($process, 9);
				break;
			}
			usleep(10000);
		}
		foreach([$pipes[1], $pipes[2]] as $pipe){
			if(is_resource($pipe)){ fclose($pipe); }
		}
		proc_close($process);
	});
	$line = readOffererLine($pipes[1]);
	check(str_starts_with($line, 'OFFER '), 'offerer protocol');
	$sdp = hex2bin(substr($line, 6));
	check(is_string($sdp), 'offer encoding');
	return [$process, $pipes, $sdp];
}

function stopOfferer($process, array $pipes) : void{
	fwrite($pipes[0], "CLOSE\n");
	fclose($pipes[0]);
	$status = null;
	waitFor(function() use($process, &$status){
		$status = proc_get_status($process);
		return !$status['running'];
	});
	$error = processOutput($pipes[2]);
	fclose($pipes[1]);
	fclose($pipes[2]);
	$closed = proc_close($process);
	$code = $status['exitcode'] >= 0 ? $status['exitcode'] : $closed;
	check($code === 0 && $error === '', 'offerer shutdown failed: ' . $error);
}

function rtcError(callable $operation, int $code) : void{
	try{
		$operation();
		throw new RuntimeException('expected RTC error ' . $code);
	}catch(amber\rtc\RtcException $error){
		check($error->getCode() === $code, 'RTC code ' . $error->getCode() . ' expected ' . $code);
	}
}
