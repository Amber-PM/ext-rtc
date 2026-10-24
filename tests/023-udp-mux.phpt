--TEST--
rtc ICE UDP mux shares one socket across two isolated peers
--FILE--
<?php
declare(strict_types=1);
require __DIR__ . '/support/network.php';

use amber\rtc\{ConnectionState, GatheringState, PeerConnection, PeerConnectionOptions, RtcRuntime};

// below the os ephemeral ranges, so later sockets cannot be handed the same port
function freePort() : int{
	static $given = [];
	for($attempt = 0; $attempt < 1000; ++$attempt){
		$port = random_int(20000, 32000);
		$probe = isset($given[$port]) ? false : @stream_socket_server('udp://127.0.0.1:' . $port, $code, $error, STREAM_SERVER_BIND);
		if(is_resource($probe)){
			fclose($probe);
			return $given[$port] = $port;
		}
	}
	throw new RuntimeException('find available UDP port');
}
function portHeld(int $port) : bool{
	$socket = @stream_socket_server('udp://127.0.0.1:' . $port, $code, $error, STREAM_SERVER_BIND);
	if(!is_resource($socket)){ return true; }
	fclose($socket);
	return false;
}
// every UDP socket this process owns, from procfs where it exists
function ownUdpSockets() : ?array{
	if(!is_dir('/proc/self/fd') || !is_readable('/proc/net/udp')){ return null; }
	$inodes = [];
	foreach(glob('/proc/self/fd/*') as $fd){
		$link = @readlink($fd);
		if(is_string($link) && preg_match('/^socket:\[(\d+)\]$/', $link, $m)){ $inodes[$m[1]] = true; }
	}
	$sockets = [];
	foreach(['/proc/net/udp', '/proc/net/udp6'] as $table){
		foreach(array_slice(file($table) ?: [], 1) as $line){
			$fields = preg_split('/\s+/', trim($line));
			if(isset($inodes[$fields[9]])){
				[$address, $port] = explode(':', $fields[1]);
				$sockets[] = (strlen($address) === 8 ? long2ip((int) hexdec(implode('', array_reverse(str_split($address, 2))))) : $address) . ':' . hexdec($port);
			}
		}
	}
	return $sockets;
}
function muxCommand(array $pipes, string $command) : string{
	fwrite($pipes[0], $command . "\n");
	return readOffererLine($pipes[1]);
}

$port = freePort();
$options = PeerConnectionOptions::create()
	->setBindAddress('127.0.0.1')
	->setPortRange($port, $port)
	->setIceUdpMuxEnabled(true);

$peers = [];
$offerers = [];
$channels = [];
for($i = 0; $i < 2; ++$i){
	[$process, $pipes, $offer] = offerer();
	$offerers[] = [$process, $pipes];
	$peer = new PeerConnection($options);
	$peers[] = $peer;
	$peer->setRemoteOffer($offer);
	waitFor(fn() => $peer->getGatheringState() === GatheringState::COMPLETE);
	$answer = $peer->getLocalDescription();
	check(is_string($answer) && preg_match_all('/^a=candidate:\S+ \d+ \S+ \d+ (\S+) ([0-9]+) typ host/m', $answer, $matches, PREG_SET_ORDER) === 1, 'one host ICE candidate');
	check($matches[0][1] === '127.0.0.1' && (int) $matches[0][2] === $port, 'mux candidate must use the shared bind and port');
	fwrite($pipes[0], bin2hex($answer) . "\n");
}
foreach($offerers as [$process, $pipes]){
	check(readOffererLine($pipes[1]) === 'STATE connected', 'concurrent mux handshake');
}
foreach($peers as $i => $peer){
	check($peer->getState() === ConnectionState::CONNECTED, 'mux peer connected');
	check($peer->getLocalAddress() === '127.0.0.1:' . $port, 'selected pair uses the shared socket');
	waitFor(function() use ($peer, &$channels, $i) : bool{
		foreach($peer->pollDataChannels() as $channel){
			if($channel->getLabel() === 'transport-test'){ $channels[$i] = $channel; }
		}
		return isset($channels[$i]) && $channels[$i]->isOpen();
	});
}
check($peers[0]->getRemoteFingerprint() !== $peers[1]->getRemoteFingerprint(), 'separate DTLS sessions');
$sockets = ownUdpSockets();
check($sockets === null || $sockets === ['127.0.0.1:' . $port], 'exactly one shared UDP socket');
check(portHeld($port), 'shared socket holds the port');

foreach($peers as $i => $peer){
	$payload = "mux-answer-" . $i;
	check($channels[$i]->send($payload), 'mux send');
	waitFor(function() use ($offerers, $i, $payload) : bool{
		$line = muxCommand($offerers[$i][1], 'RECEIVE 0');
		if($line === 'EMPTY'){ return false; }
		check($line === 'DATA ' . bin2hex($payload), 'mux payload');
		return true;
	});
	check(muxCommand($offerers[1 - $i][1], 'RECEIVE 0') === 'EMPTY', 'answer traffic isolation');
	$reply = "mux-offer-" . $i;
	check(muxCommand($offerers[$i][1], 'SEND 0 ' . bin2hex($reply)) === 'SENT', 'offerer send');
	waitFor(fn() => $channels[$i]->peek() !== null);
	check($channels[$i]->receive() === $reply, 'offer payload');
	check($channels[1 - $i]->receive() === null, 'offer traffic isolation');
}

$peers[0]->close();
stopOfferer(...$offerers[0]);
check($channels[1]->isOpen() && $peers[1]->getState() === ConnectionState::CONNECTED, 'remaining peer survives');
check($channels[1]->send('still-alive'), 'remaining peer send');
waitFor(function() use ($offerers) : bool{
	$line = muxCommand($offerers[1][1], 'RECEIVE 0');
	if($line === 'EMPTY'){ return false; }
	check($line === 'DATA ' . bin2hex('still-alive'), 'remaining peer unaffected');
	return true;
});
check(muxCommand($offerers[1][1], 'SEND 0 ' . bin2hex('back')) === 'SENT', 'remaining offerer send');
waitFor(fn() => $channels[1]->peek() !== null);
check($channels[1]->receive() === 'back', 'remaining peer receives');
check(portHeld($port), 'shared socket stays with the remaining peer');

$peers[1]->close();
stopOfferer(...$offerers[1]);
unset($channels, $peers);
waitFor(fn() => !portHeld($port));
$stats = RtcRuntime::getStats();
check($stats['activePeers'] === 0 && $stats['activeChannels'] === 0, 'peer cleanup');
check((ownUdpSockets() ?? []) === [], 'shared socket released');
echo "ICE UDP mux two peers OK\n";
?>
--EXPECT--
ICE UDP mux two peers OK
