<?php
// SPDX-License-Identifier: MIT
declare(strict_types=1);

// negative checks: quality.php must pass on a clean copy and fail on real stray artifacts
$root = dirname(__DIR__);

function run(array $command, string $cwd) : array{
	$process = proc_open($command, [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes, $cwd);
	if(!is_resource($process)){ throw new RuntimeException('Unable to start ' . $command[0]); }
	$output = stream_get_contents($pipes[1]) . stream_get_contents($pipes[2]);
	fclose($pipes[1]);
	fclose($pipes[2]);
	return [proc_close($process), $output];
}

function remove(string $path) : void{
	if(!is_dir($path) || is_link($path)){ @unlink($path); return; }
	foreach(scandir($path) as $entry){
		if($entry !== '.' && $entry !== '..'){ remove("$path/$entry"); }
	}
	@rmdir($path);
}

function put(string $path, string $text) : void{
	if(!is_dir(dirname($path))){ mkdir(dirname($path), 0777, true); }
	file_put_contents($path, $text);
}

[$status, $inventory] = run(['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard'], $root);
if($status !== 0){ throw new RuntimeException($inventory); }
$copy = sys_get_temp_dir() . '/rtc-quality-' . bin2hex(random_bytes(4));
foreach(array_filter(explode("\0", $inventory)) as $file){
	if(is_file("$root/$file")){ put("$copy/$file", file_get_contents("$root/$file")); }
}
[$status, $output] = run(['git', 'init', '-q'], $copy);
if($status !== 0){ remove($copy); throw new RuntimeException($output); }

$failures = [];
$check = function(string $name, bool $shouldFail, ?string $needle) use ($copy, &$failures) : void{
	[$status, $output] = run([PHP_BINARY, '-n', 'scripts/quality.php'], $copy);
	$bad = $shouldFail ? ($status === 0 || ($needle !== null && !str_contains($output, $needle))) : $status !== 0;
	if($bad){ $failures[] = "$name: exit $status, " . trim($output); }
	echo ($bad ? 'FAIL' : 'PASS') . ": $name\n";
};

try{
	$check('clean tree', false, null);
	put("$copy/.build-stress/CMakeFiles/client.o", 'x');
	put("$copy/.build-stress/gen.cpp", "int x;\n");
	$check('ignored build-stress directory', false, null);
	[$status, $output] = run(['git', 'add', '-f', '.build-stress/gen.cpp'], $copy);
	$check('force-added build-stress source', true, '.build-stress/gen.cpp');
	run(['git', 'rm', '-q', '--cached', '-f', '.build-stress/gen.cpp'], $copy);
	remove("$copy/.build-stress");
	put("$copy/tests/stray.o", 'x');
	$check('stray object file', true, 'tests/stray.o');
	unlink("$copy/tests/stray.o");
	put("$copy/tests/native/leak.cpp", '// ' . 'C:' . '\\' . 'Users' . '\\' . "someone\\x\n");
	$check('windows user path', true, 'tests/native/leak.cpp');
	put("$copy/tests/native/leak.cpp", '// /ho' . "me/someone/work\n");
	$check('linux home path', true, 'tests/native/leak.cpp');
	unlink("$copy/tests/native/leak.cpp");
	put("$copy/tests/stress/__pycache__/a.pyc", 'x');
	$check('ignored pycache', false, null);
	run(['git', 'add', '-f', 'tests/stress/__pycache__/a.pyc'], $copy);
	$check('force-added pyc', true, 'a.pyc');
}finally{
	remove($copy);
}
if($failures !== []){ fwrite(STDERR, implode("\n", $failures) . "\n"); exit(1); }
echo "PASS: quality preflight negative checks\n";
