--TEST--
rtc test process polling preserves deadlines and binary output
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
require __DIR__ . '/support/network.php';

[$process, $pipes] = startTestProcess([PHP_BINARY, '-n', '-r',
	'usleep(3000000); fwrite(STDOUT, "data\0\n"); fwrite(STDERR, "diagnostic");'
]);
try{
	fclose($pipes[0]);
	$start = hrtime(true);
	check(processOutput($pipes[1]) === '', 'child has not written yet');
	check(hrtime(true) - $start < 1_000_000_000, 'output poll must not wait for child output');
	$timedOut = false;
	try{
		waitFor(fn() => processOutput($pipes[1]) !== '', 1);
	}catch(RuntimeException $error){
		check($error->getMessage() === 'network wait timed out', 'expected deadline');
		$timedOut = true;
	}
	check($timedOut && hrtime(true) - $start < 2_000_000_000, 'deadline remains effective');
	$status = null;
	waitFor(function() use($process, &$status){
		$status = proc_get_status($process);
		return !$status['running'];
	});
	check(processOutput($pipes[1]) === "data\0\n", 'binary stdout survives an earlier eof');
	check(processOutput($pipes[1]) === '', 'output is consumed once');
	check(processOutput($pipes[2]) === 'diagnostic', 'stderr stays separate');
	fclose($pipes[1]);
	fclose($pipes[2]);
	$closed = proc_close($process);
	check(($status['exitcode'] >= 0 ? $status['exitcode'] : $closed) === 0, 'child exit');
}finally{
	if(is_resource($process)){ proc_terminate($process, 9); }
	foreach($pipes as $pipe){ if(is_resource($pipe)){ fclose($pipe); } }
	if(is_resource($process)){ proc_close($process); }
}
echo "process deadlines and output OK\n";
?>
--EXPECT--
process deadlines and output OK
