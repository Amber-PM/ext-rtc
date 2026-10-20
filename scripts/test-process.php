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

/** @return array{resource, array<int, resource>} */
function startTestProcess(array $command) : array{
	$stdout = tmpfile();
	$stderr = tmpfile();
	if($stdout === false || $stderr === false){
		if(is_resource($stdout)){ fclose($stdout); }
		if(is_resource($stderr)){ fclose($stderr); }
		throw new RuntimeException('Unable to create process output files');
	}
	try{
		// windows php pipe reads can block despite stream_set_blocking(false)
		$process = proc_open($command, [
			0 => ['pipe', 'r'],
			1 => ['file', stream_get_meta_data($stdout)['uri'], 'ab'],
			2 => ['file', stream_get_meta_data($stderr)['uri'], 'ab']
		], $pipes);
		if(!is_resource($process)){ throw new RuntimeException('Unable to start test process'); }
	}catch(Throwable $error){
		fclose($stdout);
		fclose($stderr);
		throw $error;
	}
	return [$process, [0 => $pipes[0], 1 => $stdout, 2 => $stderr]];
}

/** @param resource $stream */
function processOutput($stream) : string{
	// clear eof before polling a file that the child can still append to
	if(fseek($stream, 0, SEEK_CUR) !== 0){ throw new RuntimeException('Unable to seek process output'); }
	$output = stream_get_contents($stream);
	if($output === false){ throw new RuntimeException('Unable to read process output'); }
	return $output;
}
