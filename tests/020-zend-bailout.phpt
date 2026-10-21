--TEST--
rtc Zend allocation bailout leaves channel queues and transport locks releasable
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
$module=getenv('RTC_TEST_MODULE');check(is_string($module) && is_file($module),'native module path required');
foreach(['receive','peek'] as $mode){
	[$process,$pipes]=startTestProcess([PHP_BINARY,'-n','-d','extension='.$module,'-d','memory_limit=16M','-d','display_errors=stderr','-d','log_errors=0',__DIR__.'/support/bailout.php',$mode]);
	fclose($pipes[0]);$out=$err='';$status=null;
	waitFor(function()use($process,$pipes,&$out,&$err,&$status){$out.=processOutput($pipes[1]);$err.=processOutput($pipes[2]);$status=proc_get_status($process);return !$status['running'];});
	$out.=processOutput($pipes[1]);$err.=processOutput($pipes[2]);fclose($pipes[1]);fclose($pipes[2]);$closed=proc_close($process);
	$code=$status['exitcode']>=0?$status['exitcode']:$closed;
	check($code===255 && $out==="TRIGGER\nCLEAN\n" && str_contains($err,'Allowed memory size') && !str_contains($err,'SHUTDOWN_FAILURE'),$mode.' bailout cleanup failed: '.$out.$err);
}
echo "Zend bailout cleanup OK\n";
?>
--EXPECT--
Zend bailout cleanup OK
