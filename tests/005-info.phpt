--TEST--
rtc module information reports backend and actual PHP ABI
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
ob_start();
(new ReflectionExtension('rtc'))->info();
$info = ob_get_clean();
foreach(['Compiled libdatachannel version => 0.24.6', 'libdatachannel commit => 6b1e2e620f1e37f0eafeee702eaea0043cb305fd', 'Media => disabled (NO_MEDIA=ON)', 'Thread safety => ' . (PHP_ZTS ? 'ZTS' : 'NTS'), 'PHP API => '] as $needle){
	var_dump(strpos($info, $needle) !== false);
}
var_dump((new ReflectionExtension('rtc'))->getVersion() === '0.1.0-dev');
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
