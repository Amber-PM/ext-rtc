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

function check(bool $condition, string $message) : void{
	if(!$condition){
		fwrite(STDERR, "FAIL: $message\n");
		exit(1);
	}
}

check(!extension_loaded('rtc'), 'run contract checks without the extension');
$root = dirname(__DIR__, 2);
foreach(['runtime.stub.php', 'rtc.stub.php'] as $file){
	check(is_file("$root/stubs/$file"), "missing contract $file");
	require "$root/stubs/$file";
}
$contracts = [
	'PeerConnectionOptions' => [
		'create' => [[], 'self'],
		'setMaxMessageSize' => [['int'], 'self'],
		'setMaxReceiveQueueSize' => [['int'], 'self'],
		'setMaxReceiveQueueMessages' => [['int'], 'self'],
		'setMaxSendQueueSize' => [['int'], 'self'],
		'setMaxRemoteDescriptionSize' => [['int'], 'self'],
		'setMaxPendingDataChannels' => [['int'], 'self'],
		'setIceTcpEnabled' => [['bool'], 'self'],
		'setPortRange' => [['int', 'int'], 'self'],
		'setBindAddress' => [['?string'], 'self'],
		'setIceUdpMuxEnabled' => [['bool'], 'self'],
		'setIceServers' => [['amber\rtc\IceServer'], 'self']
	],
	'DataChannelOptions' => [
		'create' => [[], 'self'],
		'setUnordered' => [['bool'], 'self'],
		'setMaxRetransmits' => [['?int'], 'self'],
		'setMaxPacketLifeTime' => [['?int'], 'self'],
		'setProtocol' => [['string'], 'self']
	],
	'PeerConnection' => [
		'__construct' => [['amber\rtc\PeerConnectionOptions'], ''],
		'setRemoteOffer' => [['string'], 'void'],
		'setRemoteAnswer' => [['string'], 'void'],
		'getGatheringState' => [[], 'amber\rtc\GatheringState'],
		'getLocalDescription' => [[], '?string'],
		'getState' => [[], 'amber\rtc\ConnectionState'],
		'getFailureState' => [[], '?amber\rtc\ConnectionState'],
		'getRemoteFingerprint' => [[], '?string'],
		'createDataChannel' => [['string', '?amber\rtc\DataChannelOptions'], 'amber\rtc\DataChannel'],
		'pollDataChannels' => [[], 'array'],
		'pollLocalCandidates' => [[], 'array'],
		'addRemoteCandidate' => [['amber\rtc\IceCandidate'], 'void'],
		'getLocalAddress' => [[], '?string'],
		'getRemoteAddress' => [[], '?string'],
		'getStats' => [[], 'array'],
		'close' => [[], 'void']
	],
	'DataChannel' => [
		'getLabel' => [[], 'string'], 'getProtocol' => [[], 'string'],
		'isOpen' => [[], 'bool'], 'isClosed' => [[], 'bool'], 'isUnordered' => [[], 'bool'],
		'getMaxRetransmits' => [[], '?int'], 'getMaxPacketLifeTime' => [[], '?int'],
		'getMaxMessageSize' => [[], 'int'], 'getBufferedAmount' => [[], 'int'],
		'getAvailableAmount' => [[], 'int'], 'getQueuedMessageCount' => [[], 'int'],
		'send' => [['string'], 'bool'], 'peek' => [[], '?string'],
		'receive' => [[], '?string'], 'close' => [[], 'void']
	],
	'IceServer' => [
		'stun' => [['string', 'int'], 'self'],
		'turn' => [['string', 'int', 'string', 'string', 'string'], 'self']
	],
	'IceCandidate' => [
		'__construct' => [['string', 'string'], ''],
		'getCandidate' => [[], 'string'], 'getMid' => [[], 'string']
	],
	'RtcRuntime' => [
		'getVersion' => [[], 'string'], 'getApiVersion' => [[], 'int'],
		'getCapabilities' => [[], 'array'], 'getStats' => [[], 'array']
	]
];
$count = 0;
foreach($contracts as $name => $methods){
	$class = new ReflectionClass('amber\\rtc\\' . $name);
	check($class->isFinal(), "$name must be final");
	foreach($methods as $method => [$parameters, $return]){
		check($class->hasMethod($method), "$name::$method absent");
		$actual = $class->getMethod($method);
		check($actual->isPublic(), "$name::$method visibility");
		$type = (string) $actual->getReturnType();
		check($type === $return || ($return === 'self' && $type === $class->getName()), "$name::$method return");
		check(array_map(static fn(ReflectionParameter $p) : string => (string) $p->getType(), $actual->getParameters()) === $parameters, "$name::$method parameters");
		++$count;
	}
}
check((new ReflectionMethod(amber\rtc\PeerConnectionOptions::class, 'setIceServers'))->getParameters()[0]->isVariadic(), 'ICE servers variadic');
check((new ReflectionMethod(amber\rtc\IceServer::class, 'stun'))->getParameters()[1]->getDefaultValue() === 3478, 'STUN port');
check((new ReflectionMethod(amber\rtc\IceServer::class, 'turn'))->getParameters()[4]->getDefaultValue() === 'udp', 'TURN transport');
check((new ReflectionMethod(amber\rtc\PeerConnection::class, 'createDataChannel'))->getParameters()[1]->getDefaultValue() === null, 'channel options default');
foreach(['DataChannel', 'RtcRuntime', 'IceServer'] as $name){
	check((new ReflectionClass('amber\\rtc\\' . $name))->getConstructor()?->isPrivate() === true, "$name constructor");
}
foreach(['getVersion', 'getApiVersion', 'getCapabilities', 'getStats'] as $method){
	check((new ReflectionMethod(amber\rtc\RtcRuntime::class, $method))->isStatic(), "runtime::$method static");
}
check(array_column(amber\rtc\ConnectionState::cases(), 'name') === ['NEW', 'CONNECTING', 'CONNECTED', 'DISCONNECTED', 'FAILED', 'CLOSED'], 'connection states');
check(array_column(amber\rtc\GatheringState::cases(), 'name') === ['NEW', 'IN_PROGRESS', 'COMPLETE'], 'gathering states');
$errors = ['INVALID_ARGUMENT', 'CLOSED', 'INVALID_STATE', 'INVALID_SDP', 'SDP_TOO_LARGE', 'PAYLOAD_TOO_LARGE', 'CANDIDATE_LIMIT', 'CHANNEL_LIMIT', 'RECEIVE_OVERFLOW', 'BUFFER_BUDGET', 'BACKEND_FAILURE', 'SHUTDOWN_FAILURE'];
foreach($errors as $index => $name){
	check(constant('amber\\rtc\\RtcException::' . $name) === $index + 1, "error $name");
}
check(is_subclass_of(amber\rtc\RtcException::class, RuntimeException::class), 'exception base');
echo "PASS: $count public signatures, defaults, states and errors\n";

