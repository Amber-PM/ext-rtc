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

if($argc !== 3){
	fwrite(STDERR, "Usage: php -n scripts/assert-phpt-summary.php EXPECTED_COUNT RUN_TESTS_LOG\n");
	exit(2);
}

$expected = preg_match('/^[1-9][0-9]*$/D', $argv[1]) ? (int)$argv[1] : false;
$log = file_get_contents($argv[2]);
if($expected === false || (string)$expected !== $argv[1] || $log === false){
	fwrite(STDERR, "FAIL: invalid expected PHPT count or unreadable run-tests log\n");
	exit(2);
}

$patterns = [
	'total' => '/^Number of tests\s*:\s*(\d+)\s+(\d+)\s*$/m',
	'skipped' => '/^Tests skipped\s*:\s*(\d+)\s*\(/m',
	'warned' => '/^Tests warned\s*:\s*(\d+)\s*\(/m',
	'failed' => '/^Tests failed\s*:\s*(\d+)\s*\(/m',
	'passed' => '/^Tests passed\s*:\s*(\d+)\s*\(/m'
];
$counts = [];
foreach($patterns as $field => $pattern){
	if(!preg_match($pattern, $log, $matches)){
		fwrite(STDERR, "FAIL: run-tests summary missing $field count\n");
		exit(1);
	}
	$counts[$field] = array_map('intval', array_slice($matches, 1));
}

if($counts['total'][0] !== $expected || $counts['total'][1] !== $expected ||
	$counts['skipped'][0] !== 0 || $counts['warned'][0] !== 0 ||
	$counts['failed'][0] !== 0 || $counts['passed'][0] !== $expected){
	fwrite(STDERR, sprintf(
		"FAIL: PHPT expected=%d selected=%d passed=%d skipped=%d warned=%d failed=%d\n",
		$expected,
		$counts['total'][1],
		$counts['passed'][0],
		$counts['skipped'][0],
		$counts['warned'][0],
		$counts['failed'][0]
	));
	exit(1);
}

printf("PASS: %d PHPT passed; no skips, warnings, or failures\n", $expected);
