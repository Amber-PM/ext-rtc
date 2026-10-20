--TEST--
rtc values validate immediately and remain opaque
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
use amber\rtc\{PeerConnectionOptions as Options, IceServer, IceCandidate, RtcException};
function rejects(callable $f, int $code = 1) : void{
	try{ $f(); throw new RuntimeException('accepted invalid input'); }
	catch(RtcException $e){ if($e->getCode() !== $code){ throw $e; } }
}
$o = Options::create();
foreach(['setMaxMessageSize', 'setMaxReceiveQueueSize', 'setMaxReceiveQueueMessages', 'setMaxSendQueueSize', 'setMaxRemoteDescriptionSize', 'setMaxPendingDataChannels'] as $method){
	foreach([0, -1, PHP_INT_MIN] as $n){ rejects(fn() => $o->$method($n)); }
	if($o->$method(1) !== $o){ throw new RuntimeException('setter identity'); }
}
rejects(fn() => $o->setMaxMessageSize(268435457));
rejects(fn() => $o->setMaxPendingDataChannels(17));
foreach([[0,0], [2,1], [1,65536]] as [$a,$b]){ rejects(fn() => $o->setPortRange($a,$b)); }
foreach(['localhost', '127.0.0.1:1234', '', "127.0.0.1\0"] as $ip){ rejects(fn() => $o->setBindAddress($ip)); }
$o->setBindAddress('::1')->setBindAddress(null)->setPortRange(1,65535);
foreach(['', 'stun:host', 'host:123', 'a b', "a\0b", '-bad.test', str_repeat('a',254)] as $host){ rejects(fn() => IceServer::stun($host)); }
IceServer::stun('example.com'); IceServer::stun('::1');
rejects(fn() => IceServer::stun('localhost', 0));
rejects(fn() => IceServer::turn('localhost',3478,'u','p','TCP'));
rejects(fn() => IceServer::turn('localhost',3478,"u\0",'p'));
rejects(fn() => $o->setIceServers(...array_fill(0,17,IceServer::stun('localhost'))));
$candidate = 'candidate:1 1 UDP 2122260223 127.0.0.1 5000 typ host';
$c = new IceCandidate($candidate,'0');
if($c->getCandidate() !== $candidate || $c->getMid() !== '0'){ throw new RuntimeException('candidate getters'); }
foreach(['', 'garbage', "candidate:1\r\n", str_repeat('x',4097), 'candidate:1 1 UDP 1 127.0.0.1 65536 typ host'] as $value){ rejects(fn() => new IceCandidate($value,'0')); }
rejects(fn() => new IceCandidate($candidate,''));
$secret = IceServer::turn('localhost',3478,'private-user','private-password');
ob_start(); var_dump($secret); $debug = ob_get_clean();
if(str_contains($debug,'private-')){ throw new RuntimeException('credential leak'); }
foreach([$o,$c,$secret] as $value){
	foreach([fn() => clone $value, fn() => serialize($value), function() use($value){ $value->x = 1; }] as $op){
		try{ $op(); throw new RuntimeException('opaque object bypass'); }catch(Error|Exception $e){ if($e instanceof RuntimeException){ throw $e; } }
	}
}
echo "values OK\n";
?>
--EXPECT--
values OK
