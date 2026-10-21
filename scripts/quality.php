<?php
// SPDX-License-Identifier: MIT
declare(strict_types=1);

$root = dirname(__DIR__);
$process = proc_open(['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard'], [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes, $root);
if(!is_resource($process)){ throw new RuntimeException('Unable to inspect repository inventory'); }
$inventory = stream_get_contents($pipes[1]);
$errors = stream_get_contents($pipes[2]);
fclose($pipes[1]);
fclose($pipes[2]);
if(proc_close($process) !== 0){ throw new RuntimeException($errors); }
$files = array_fill_keys(array_filter(explode("\0", $inventory)), true);
$failures = [];
$checked = 0;
foreach(array_keys($files) as $file){
	if(!file_exists("$root/$file")){
		unset($files[$file]);
		continue;
	}
	if(preg_match('~(^|/)(?:\.build|\.agents|\.superpowers|__pycache__|AGENTS\.md)(/|$)|\.(?:so|dll|exe|o|obj|log|out|diff|pyc|tmp\d*)$~i', $file)){
		$failures[] = "$file: private or generated artifact in candidate inventory";
	}
	if(!preg_match('~\.(?:cpp|c|h|php|phpt|sh|ps1|py|m4|w32|yml|json|cmake)$|(^|/)CMakeLists\.txt$~', $file)){ continue; }
	$text = file_get_contents("$root/$file");
	++$checked;
	if(str_starts_with($text, "\xEF\xBB\xBF")){ $failures[] = "$file: UTF-8 BOM"; }
	if(str_contains($text, "\r\n") && preg_match('/(?<!\r)\n/', $text)){ $failures[] = "$file: mixed LF/CRLF"; }
	if(str_ends_with($file, '.sh') && str_contains($text, "\r")){ $failures[] = "$file: shell scripts require LF"; }
	if($text !== '' && !str_ends_with($text, "\n")){ $failures[] = "$file: missing final newline"; }
	foreach(explode("\n", str_replace("\r\n", "\n", $text)) as $line => $value){
		if(preg_match('/[\t ]+$/', $value)){ $failures[] = "$file:" . ($line + 1) . ': trailing whitespace'; }
	}
	if(preg_match('~(?:\.build/(?:teardown-fix|teardown-integration)|[A-Z]:[\\/]Users[\\/])~', $text)){
		$failures[] = "$file: private workspace dependency";
	}
}
$lock = json_decode(file_get_contents("$root/build/dependencies.lock.json"), true, 512, JSON_THROW_ON_ERROR);
foreach($lock['libdatachannel']['patches'] as $patch){
	if(!isset($files[$patch['path']]) || hash_file('sha256', "$root/{$patch['path']}") !== $patch['sha256']){
		$failures[] = $patch['path'] . ': missing or incorrect locked patch';
	}
}
$srcIterator = new RecursiveIteratorIterator(new RecursiveDirectoryIterator("$root/src", FilesystemIterator::SKIP_DOTS));
foreach($srcIterator as $fileInfo){
	if(!$fileInfo->isFile()){ continue; }
	$ext = $fileInfo->getExtension();
	if($ext !== 'cpp' && $ext !== 'h'){ continue; }
	$rel = str_replace('\\', '/', substr($fileInfo->getPathname(), strlen($root) + 1));
	if(!isset($files[$rel])){ $failures[] = "$rel: excluded required source"; }
	if($ext === 'cpp'){
		$m4Rel = $rel;
		$w32Rel = str_replace('/', '\\\\', $rel);
		foreach(['config.m4' => $m4Rel, 'config.w32' => $w32Rel] as $config => $entry){
			if(!str_contains(file_get_contents("$root/$config"), $entry)){ $failures[] = "$config: missing $entry"; }
		}
	}
}
foreach(['scripts/*.{php,sh,ps1}', 'tests/native/*.{cpp,c,h}', 'tests/support/*.php', 'tests/*.phpt', 'stubs/*.php'] as $pattern){
	foreach(glob("$root/$pattern", GLOB_BRACE) as $source){
		$file = str_replace('\\', '/', substr($source, strlen($root) + 1));
		if(!isset($files[$file])){ $failures[] = "$file: excluded required test/build source"; }
	}
}
if($failures !== []){ fwrite(STDERR, implode("\n", $failures) . "\n"); exit(1); }
echo "PASS: $checked source/config files, candidate inventory and exact backend patch hashes\n";
