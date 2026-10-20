--TEST--
rtc runtime exposes native version and typed static information
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
$r = new ReflectionClass(amber\rtc\RtcRuntime::class);
var_dump($r->isFinal(), $r->getExtensionName(), $r->getConstructor()->isPrivate());
var_dump(amber\rtc\RtcRuntime::getVersion(), amber\rtc\RtcRuntime::getApiVersion());
foreach(['getVersion' => 'string', 'getApiVersion' => 'int', 'getCapabilities' => 'array', 'getStats' => 'array'] as $name => $type){
	$m = $r->getMethod($name);
	var_dump($m->isPublic() && $m->isStatic() && $m->getNumberOfParameters() === 0 && (string) $m->getReturnType() === $type);
}
try{ amber\rtc\RtcRuntime::getVersion(1); }catch(ArgumentCountError $e){ echo "argument rejected\n"; }
?>
--EXPECT--
bool(true)
string(3) "rtc"
bool(true)
string(9) "0.1.0-dev"
int(1)
bool(true)
bool(true)
bool(true)
bool(true)
argument rejected
