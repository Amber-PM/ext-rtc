--TEST--
rtc ICE UDP mux rejects incompatible, busy and still-releasing sockets explicitly
--FILE--
<?php
declare(strict_types=1);
require __DIR__ . '/support/network.php';

use amber\rtc\{GatheringState, IceServer, PeerConnection, PeerConnectionOptions as Options, RtcException, RtcRuntime};

function rejects(callable $operation, string $message) : void{
	try{
		$operation();
	}catch(RtcException $e){
		check($e->getCode() === RtcException::INVALID_ARGUMENT, 'invalid argument code');
		check($e->getMessage() === $message, 'error message: ' . $e->getMessage());
		return;
	}
	throw new RuntimeException('accepted: ' . $message);
}
// below the os  ephemeral ranges, so later sockets cannot be handed the same port
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
			if(isset($inodes[$fields[9]])){ $sockets[] = $fields[1]; }
		}
	}
	return $sockets;
}
function mux(?string $bind, int $port) : Options{
	return Options::create()->setBindAddress($bind)->setPortRange($port, $port)->setIceUdpMuxEnabled(true);
}

$port = freePort();
rejects(fn() => new PeerConnection(Options::create()->setIceUdpMuxEnabled(true)), 'ICE UDP mux requires a single fixed port');
rejects(fn() => new PeerConnection(Options::create()->setPortRange($port, $port + 1)->setIceUdpMuxEnabled(true)), 'ICE UDP mux requires a single fixed port');
rejects(fn() => new PeerConnection(mux('127.0.0.1', $port)->setIceServers(IceServer::turn('127.0.0.1', 3478, 'u', 'p'))), 'ICE UDP mux does not support TURN');
rejects(fn() => new PeerConnection(mux('127.0.0.1', $port)->setIceTcpEnabled(true)), 'ICE TCP is not supported by this backend');
$stun = new PeerConnection(mux('127.0.0.1', $port)->setIceServers(IceServer::stun('127.0.0.1')));
$stun->close();

$port = freePort();
$busy = stream_socket_server('udp://127.0.0.1:' . $port, $code, $error, STREAM_SERVER_BIND);
check(is_resource($busy), 'occupy the mux port');
rejects(fn() => new PeerConnection(mux('127.0.0.1', $port)), 'ICE UDP mux port is unavailable');
check(RtcRuntime::getStats()['activePeers'] === 0, 'failed creation is not admitted');
fclose($busy);

$port = freePort();
$first = new PeerConnection(mux('127.0.0.1', $port));
$same = new PeerConnection(mux('127.0.0.1', $port));
rejects(fn() => new PeerConnection(mux(null, $port)), 'ICE UDP mux port is shared with a different bind address');
$first->close();
rejects(fn() => new PeerConnection(mux(null, $port)), 'ICE UDP mux port is shared with a different bind address');
unset($same);
// neither peer gathered, so the backend never opened the socket
$wildcard = new PeerConnection(mux(null, $port));
$wildcard->close();

$port = freePort();
$offerer = new PeerConnection(Options::create()->setBindAddress('127.0.0.1'));
$channel = $offerer->createDataChannel('mux');
waitFor(fn() => $offerer->getGatheringState() === GatheringState::COMPLETE);
$gathered = new PeerConnection(mux('127.0.0.1', $port));
$gathered->setRemoteOffer($offerer->getLocalDescription());
waitFor(fn() => $gathered->getGatheringState() === GatheringState::COMPLETE);
check(str_contains($gathered->getLocalDescription(), ' 127.0.0.1 ' . $port . ' typ host'), 'mux socket opened');
$gathered->close();
$channel->close();
$offerer->close();
// the backend closes the shared socket asynchronously, a different bind must wait instead of joining it
$accepted = null;
waitFor(function() use ($port, &$accepted) : bool{
	try{
		$accepted = new PeerConnection(mux(null, $port));
		return true;
	}catch(RtcException $e){
		check($e->getCode() === RtcException::INVALID_ARGUMENT && $e->getMessage() === 'ICE UDP mux port is still releasing a different bind address', 'releasing error');
		return false;
	}
});
$accepted->close();
unset($accepted, $gathered, $offerer, $channel, $wildcard, $first, $stun);
$used = [$port];

function held(string $host, int $port) : bool{
	$socket = @stream_socket_server('udp://' . $host . ':' . $port, $code, $error, STREAM_SERVER_BIND);
	if(!is_resource($socket)){ return true; }
	fclose($socket);
	return false;
}
function gatheredCandidates(PeerConnection $answer) : array{
	$offerer = new PeerConnection(Options::create()->setBindAddress('127.0.0.1'));
	$channel = $offerer->createDataChannel('mux');
	waitFor(fn() => $offerer->getGatheringState() === GatheringState::COMPLETE);
	$answer->setRemoteOffer($offerer->getLocalDescription());
	waitFor(fn() => $answer->getGatheringState() === GatheringState::COMPLETE);
	$channel->close();
	$offerer->close();
	preg_match_all('/^a=candidate:\S+ \d+ \S+ \d+ (\S+) ([0-9]+) typ host/m', $answer->getLocalDescription(), $matches, PREG_SET_ORDER);
	return array_map(fn(array $m) : string => $m[1] . ':' . $m[2], $matches);
}

// quick reconnects reuse the same port and bind while the previous socket may still be closing
$port = $used[] = freePort();
for($i = 0; $i < 20; ++$i){
	$peer = new PeerConnection(mux('127.0.0.1', $port));
	check(gatheredCandidates($peer) === ['127.0.0.1:' . $port], 'reconnect gathers on the shared port');
	$peer->close();
}
waitFor(fn() => !held('127.0.0.1', $port));
// once the native socket is gone, an outside owner of the same port fails construction, not negotiation
$busy = stream_socket_server('udp://127.0.0.1:' . $port, $code, $error, STREAM_SERVER_BIND);
check(is_resource($busy), 'occupy the released mux port');
rejects(fn() => new PeerConnection(mux('127.0.0.1', $port)), 'ICE UDP mux port is unavailable');
check(RtcRuntime::getStats()['activePeers'] === 0, 'rejected reuse is not admitted');
fclose($busy);
$peer = new PeerConnection(mux('127.0.0.1', $port));
check(gatheredCandidates($peer) === ['127.0.0.1:' . $port], 'port usable after the outside owner leaves');
$peer->close();

// libjuice falls back to IPv4 when the dual-stack wildcard cannot bind
$port = $used[] = freePort();
$v6only = @stream_socket_server('udp://[::]:' . $port, $code, $error, STREAM_SERVER_BIND, stream_context_create(['socket' => ['ipv6_v6only' => true]]));
if(is_resource($v6only)){
	$peer = new PeerConnection(mux(null, $port));
	$candidates = gatheredCandidates($peer);
	check($candidates !== [] && array_filter($candidates, fn(string $c) : bool => !str_ends_with($c, ':' . $port)) === [], 'IPv4 wildcard fallback gathers on the mux port');
	// a foreign socket sharing the port on windows swallows libjuice's mux wakeup, stalling teardown
	fclose($v6only);
	$peer->close();
	waitFor(fn() => !held('0.0.0.0', $port));
	// windows lets a dual-stack wildcard share ports with php's server sockets depending on port history
	if(PHP_OS_FAMILY !== 'Windows'){
		$v4 = stream_socket_server('udp://0.0.0.0:' . $port, $code, $error, STREAM_SERVER_BIND);
		check(is_resource($v4), 'occupy the IPv4 wildcard');
		rejects(fn() => new PeerConnection(mux(null, $port)), 'ICE UDP mux port is unavailable');
		fclose($v4);
	}
}

// a failed negotiation leaves the peer closable and its claim releasable
$port = $used[] = freePort();
$failed = new PeerConnection(mux('127.0.0.1', $port));
try{
	$failed->setRemoteOffer("v=0\r\n");
	throw new RuntimeException('invalid offer accepted');
}catch(RtcException $e){
	check($e->getCode() === RtcException::INVALID_SDP, 'invalid SDP code');
}
$failed->close();
(new PeerConnection(mux(null, $port)))->close();

// explicit IPv6 bind carries a real connection over the shared socket
$port = $used[] = freePort();
$v6probe = @stream_socket_server('udp://[::1]:' . $port, $code, $error, STREAM_SERVER_BIND);
if(is_resource($v6probe)){
	fclose($v6probe);
	$offerer = new PeerConnection(Options::create()->setBindAddress('::1'));
	$local = $offerer->createDataChannel('v6');
	waitFor(fn() => $offerer->getGatheringState() === GatheringState::COMPLETE);
	$answer = new PeerConnection(mux('::1', $port));
	$answer->setRemoteOffer($offerer->getLocalDescription());
	waitFor(fn() => $answer->getGatheringState() === GatheringState::COMPLETE);
	check(preg_match_all('/^a=candidate:\S+ \d+ \S+ \d+ (\S+) ([0-9]+) typ host/m', $answer->getLocalDescription(), $m, PREG_SET_ORDER) === 1 && $m[0][1] === '::1' && (int) $m[0][2] === $port, 'IPv6 mux candidate');
	rejects(fn() => new PeerConnection(mux('127.0.0.1', $port)), 'ICE UDP mux port is shared with a different bind address');
	$offerer->setRemoteAnswer($answer->getLocalDescription());
	$remote = null;
	waitFor(function() use ($answer, $local, &$remote) : bool{
		foreach($answer->pollDataChannels() as $channel){ $remote = $channel; }
		return $remote !== null && $remote->isOpen() && $local->isOpen();
	});
	check($local->send("v6\x00out") && $remote->send("v6\x00back"), 'IPv6 mux send');
	waitFor(fn() => $remote->peek() !== null && $local->peek() !== null);
	check($remote->receive() === "v6\x00out" && $local->receive() === "v6\x00back", 'IPv6 mux payloads');
	$remote->close();
	$local->close();
	$answer->close();
	$offerer->close();
	unset($remote, $local, $answer, $offerer);
}

// sockets close asynchronously after their peers
waitFor(fn() => (ownUdpSockets() ?? []) === []);
// no claim survives its peers, so another bind can take every used port
foreach($used as $usedPort){
	waitFor(function() use ($usedPort) : bool{
		try{
			(new PeerConnection(mux('127.0.0.1', $usedPort)))->close();
			return true;
		}catch(RtcException $e){
			return false;
		}
	});
}
check(RtcRuntime::getStats()['activePeers'] === 0, 'all peers closed');
echo "ICE UDP mux errors OK\n";
?>
--EXPECT--
ICE UDP mux errors OK
