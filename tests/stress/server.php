<?php
/*
 * rtc stress server driver
 * php -n -d extension=rtc.so server.php --port=9000 --mode=mux --ice-port=40000 --samples=out.jsonl
 *
 * signaling over tcp, one line per message, localhost only
 *   client: HELLO client | OFFER <id> <hex sdp>
 *   ctl:    HELLO ctl    | STATS | QUIT
 *   server: ANSWER <id> <hex sdp> | REJECT <id> <reason>
 * data plane, first payload byte is the type
 *   R echo request (8 byte seq, 8 byte ts) answered with r and the same bytes
 *   D numbered data checked for order and loss
 *   CSTREAM <rate> <size> server streams D on both channels
 *   CFLOOD <size> <count> server floods F on the reliable channel as fast as the native queue accepts
 */

declare(strict_types=1);

use amber\rtc\ConnectionState;
use amber\rtc\DataChannel;
use amber\rtc\GatheringState;
use amber\rtc\PeerConnection;
use amber\rtc\PeerConnectionOptions;
use amber\rtc\RtcException;
use amber\rtc\RtcRuntime;

$args = [];
foreach(array_slice($argv, 1) as $a){
	if(preg_match('/^--([a-z0-9-]+)(?:=(.*))?$/', $a, $m)){ $args[$m[1]] = $m[2] ?? '1'; }
}
$cfg = [
	'port' => (int) ($args['port'] ?? 9000),
	'mode' => $args['mode'] ?? 'mux',
	'icePort' => (int) ($args['ice-port'] ?? 40000),
	'iceEnd' => (int) ($args['ice-end'] ?? 0),
	'bind' => $args['bind'] ?? '127.0.0.1',
	'samples' => $args['samples'] ?? '',
	'interval' => (float) ($args['interval'] ?? 1),
	'sendQueue' => (int) ($args['send-queue'] ?? 0),
	'recvQueue' => (int) ($args['recv-queue'] ?? 0),
	'recvMessages' => (int) ($args['recv-messages'] ?? 0),
	'maxMessage' => (int) ($args['max-message'] ?? 0),
	'idleUs' => (int) ($args['idle-us'] ?? 300),
	'filter' => ($args['filter'] ?? '1') !== '0',
];

function nowNs() : int{ return hrtime(true); }

// same filter as amber\nethernet\backend\ExtRtcPeer, real clients add tcp, mdns and component 2 candidates that ext-rtc rejects
function filterCandidates(string $sdp) : string{
	// aiortc offers sha-256 plus sha-384 and sha-512, ext-rtc rejects the whole sdp over the extra lines
	if(($cut = preg_replace('/^a=fingerprint:(?!sha-256 )[^\r\n]*\r?\n/mi', '', $sdp)) !== null){ $sdp = $cut; }
	return preg_replace_callback('/^a=candidate:\S+ ([12]) (\S+) \d+ (\S+) [1-9]\d{0,4} typ (?:host|srflx|prflx|relay)(?: [^\r\n]*)?(?:\r?\n|$)/m', static function(array $match) : string{
		return $match[1] === '1' && strtolower($match[2]) === 'udp' && filter_var($match[3], FILTER_VALIDATE_IP) !== false ? $match[0] : '';
	}, $sdp) ?? $sdp;
}

// glibc allocator counters through FFI when the extension is loaded, they tell a leak from memory the allocator keeps
function heapFfi() : ?\FFI{
	static $ffi = false;
	if($ffi === false){
		$ffi = null;
		if(extension_loaded('ffi')){
			try{
				$ffi = \FFI::cdef('struct mi2 { size_t arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost; }; struct mi2 mallinfo2(void); int malloc_trim(size_t pad);', 'libc.so.6');
			}catch(\Throwable $e){
				$ffi = null;
			}
		}
	}
	return $ffi;
}

function heapStats() : array{
	$ffi = heapFfi();
	if($ffi === null){ return []; }
	$m = $ffi->mallinfo2();
	return ['heap_used_kb' => intdiv($m->uordblks + $m->hblkhd, 1024), 'heap_free_kb' => intdiv($m->fordblks, 1024), 'heap_arena_kb' => intdiv($m->arena, 1024)];
}

function procStats() : array{
	static $ticks = null;
	$ticks ??= 100;
	$stat = @file_get_contents('/proc/self/stat') ?: '';
	$after = substr($stat, (int) strrpos($stat, ')') + 2);
	$f = explode(' ', $after);
	$status = @file_get_contents('/proc/self/status') ?: '';
	preg_match('/VmRSS:\s+(\d+)/', $status, $rss);
	preg_match('/VmHWM:\s+(\d+)/', $status, $hwm);
	preg_match('/Threads:\s+(\d+)/', $status, $thr);
	return [
		'rss_kb' => (int) ($rss[1] ?? 0), 'hwm_kb' => (int) ($hwm[1] ?? 0), 'threads' => (int) ($thr[1] ?? 0),
		'fds' => count(@scandir('/proc/self/fd') ?: []) - 2,
		'cpu_user_s' => ((int) ($f[11] ?? 0)) / $ticks, 'cpu_sys_s' => ((int) ($f[12] ?? 0)) / $ticks,
	] + heapStats();
}

$options = PeerConnectionOptions::create()->setBindAddress($cfg['bind']);
if($cfg['mode'] === 'mux'){
	$options->setPortRange($cfg['icePort'], $cfg['icePort'])->setIceUdpMuxEnabled(true);
}elseif($cfg['icePort'] > 0 && $cfg['iceEnd'] > 0){
	$options->setPortRange($cfg['icePort'], $cfg['iceEnd']);
}
if($cfg['sendQueue'] > 0){ $options->setMaxSendQueueSize($cfg['sendQueue']); }
if($cfg['recvQueue'] > 0){ $options->setMaxReceiveQueueSize($cfg['recvQueue']); }
if($cfg['recvMessages'] > 0){ $options->setMaxReceiveQueueMessages($cfg['recvMessages']); }
if($cfg['maxMessage'] > 0){ $options->setMaxMessageSize($cfg['maxMessage']); }

$listener = stream_socket_server('tcp://127.0.0.1:' . $cfg['port'], $errno, $errstr);
if(!$listener){ fwrite(STDERR, "listen failed: $errstr\n"); exit(2); }
stream_set_blocking($listener, false);
fwrite(STDOUT, "READY\n");
fflush(STDOUT);

$conns = [];     // socket id => [stream, buffer, role]
$peers = [];     // peer id => state
$counters = [
	'accepted' => 0, 'rejected_offers' => 0, 'answers' => 0, 'connected' => 0, 'closed' => 0, 'closed_failed' => 0,
	'echo' => 0, 'echo_dropped_false' => 0, 'echo_exceptions' => 0, 'echo_closed_races' => 0, 'send_exceptions' => [],
	'recv_rel_bad' => 0, 'recv_rel' => 0, 'recv_unrel' => 0, 'recv_unrel_lost' => 0, 'recv_unrel_reorder' => 0,
	'stream_sent_rel' => 0, 'stream_sent_unrel' => 0, 'stream_rejected_rel' => 0, 'stream_rejected_unrel' => 0,
	'flood_sent' => 0, 'flood_rejected' => 0, 'flood_budget' => 0, 'flood_done' => 0, 'flood_bytes' => 0,
	'receive_exceptions' => 0, 'poll_exceptions' => 0, 'bad_channels' => 0,
	'loop_iters' => 0, 'loop_max_ms' => 0.0, 'loop_over_10ms' => 0, 'loop_over_100ms' => 0, 'busy_ns' => 0,
	'create_max_ms' => 0.0, 'close_max_ms' => 0.0, 'create_ms_sum' => 0.0, 'close_ms_sum' => 0.0,
];
$start = hrtime(true);
$lastSample = $start;
$sampleFh = $cfg['samples'] !== '' ? fopen($cfg['samples'], 'w') : null;
$quit = false;

function countException(array &$counters, \Throwable $e) : void{
	$k = $e instanceof RtcException ? 'rtc' . $e->getCode() : get_class($e);
	$counters['send_exceptions'][$k] = ($counters['send_exceptions'][$k] ?? 0) + 1;
}

function snapshot(array $counters, array $peers, float $start) : array{
	$rt = RtcRuntime::getStats();
	$states = [];
	foreach($peers as $p){ $s = $p['ready'] ? 'ready' : ($p['answered'] ? 'answered' : 'pending'); $states[$s] = ($states[$s] ?? 0) + 1; }
	return ['t' => round((hrtime(true) - $start) / 1e9, 3), 'unix' => round(microtime(true), 3)] + procStats() + [
		'runtime' => $rt, 'peers_local' => count($peers), 'peer_states' => $states,
	] + ['counters' => $counters];
}

function sendBinary(DataChannel $ch, string $payload, array &$counters, string $dropKey) : ?bool{
	try{
		return $ch->send($payload);
	}catch(RtcException $e){
		// the client closed the peer while the echo was on its way, nothing a server can know before it sends
		if($e->getCode() === RtcException::CLOSED){ ++$counters['echo_closed_races']; return null; }
		countException($counters, $e);
		return null;
	}catch(\Throwable $e){
		countException($counters, $e);
		return null;
	}
}

function closePeer(array &$peers, array &$counters, int|string $id, bool $failed) : void{
	if(!isset($peers[$id])){ return; }
	$t = hrtime(true);
	try{ $peers[$id]['pc']->close(); }catch(\Throwable $e){ countException($counters, $e); }
	$ms = (hrtime(true) - $t) / 1e6;
	$counters['close_ms_sum'] += $ms;
	$counters['close_max_ms'] = max($counters['close_max_ms'], $ms);
	++$counters['closed'];
	if($failed){ ++$counters['closed_failed']; }
	unset($peers[$id]);
}

function handleMessage(array &$p, string $label, DataChannel $ch, string $m, array &$counters, int $now) : void{
	$reliable = $label === 'ReliableDataChannel';
	$t = $m[0] ?? '';
	if($t === 'R'){
		$races = $counters['echo_closed_races'];
		$r = sendBinary($ch, 'r' . substr($m, 1), $counters, 'echo');
		if($r === false){ ++$counters['echo_dropped_false']; }elseif($r === null){ if($counters['echo_closed_races'] === $races){ ++$counters['echo_exceptions']; } }else{ ++$counters['echo']; }
	}elseif($t === 'D' && strlen($m) >= 9){
		$seq = unpack('P', $m, 1)[1];
		if($reliable){
			++$counters['recv_rel'];
			if($seq !== $p['expRel']){ ++$counters['recv_rel_bad']; }
			$p['expRel'] = $seq + 1;
		}else{
			++$counters['recv_unrel'];
			if($seq > $p['expUnrel']){ $counters['recv_unrel_lost'] += $seq - $p['expUnrel']; $p['expUnrel'] = $seq + 1; }
			elseif($seq < $p['expUnrel']){ ++$counters['recv_unrel_reorder']; }
			else{ $p['expUnrel'] = $seq + 1; }
		}
	}elseif($t === 'C'){
		$parts = explode(' ', $m);
		if($parts[0] === 'CSTREAM' && count($parts) === 3){
			$p['stream'] = ['interval' => (int) (1e9 / max(1, (int) $parts[1])), 'size' => max(9, (int) $parts[2]), 'due' => $now, 'seqRel' => 0, 'seqUnrel' => 0];
		}elseif($parts[0] === 'CFLOOD' && count($parts) === 3){
			$p['flood'] = ['size' => max(9, (int) $parts[1]), 'left' => (int) $parts[2], 'seq' => 0];
		}
	}
}

$nonBlockingTimeout = 0;
while(true){
	$iterStart = hrtime(true);
	$work = false;

	// signaling
	$read = [$listener];
	foreach($conns as $c){ $read[] = $c['stream']; }
	$w = $e = null;
	if(@stream_select($read, $w, $e, 0, 0) > 0){
		foreach($read as $s){
			if($s === $listener){
				$c = @stream_socket_accept($listener, 0);
				if($c){ stream_set_blocking($c, false); $conns[(int) $c] = ['stream' => $c, 'buf' => '', 'role' => '']; }
				continue;
			}
			$key = (int) $s;
			$data = @fread($s, 1 << 20);
			if($data === '' || $data === false){
				if(feof($s)){ fclose($s); unset($conns[$key]); }
				continue;
			}
			$work = true;
			$conns[$key]['buf'] .= $data;
			while(($nl = strpos($conns[$key]['buf'], "\n")) !== false){
				$line = substr($conns[$key]['buf'], 0, $nl);
				$conns[$key]['buf'] = substr($conns[$key]['buf'], $nl + 1);
				$f = explode(' ', $line, 3);
				if($f[0] === 'HELLO'){ $conns[$key]['role'] = $f[1] ?? ''; continue; }
				if($f[0] === 'STATS'){ fwrite($s, json_encode(snapshot($counters, $peers, $start)) . "\n"); continue; }
				if($f[0] === 'QUIT'){ $quit = true; continue; }
				if($f[0] === 'OFFER' && count($f) === 3){
					$id = (int) $f[1];
					$t = hrtime(true);
					try{
						$pc = new PeerConnection($options);
						$sdp = (string) hex2bin($f[2]);
						if($cfg['filter']){ $sdp = filterCandidates($sdp); }
						$pc->setRemoteOffer($sdp);
						// ids are only unique per client process
						$peers[$key . ':' . $id] = ['cid' => $id, 'pc' => $pc, 'conn' => $key, 'answered' => false, 'ready' => false, 'ch' => [], 'expRel' => 0, 'expUnrel' => 0, 'stream' => null, 'flood' => null, 'born' => $t, 'failed' => false];
						++$counters['accepted'];
						unset($pc);
					}catch(\Throwable $ex){
						++$counters['rejected_offers'];
						countException($counters, $ex);
						fwrite($s, "REJECT $id " . str_replace([' ', "\n"], '_', $ex->getMessage()) . "\n");
					}
					$ms = (hrtime(true) - $t) / 1e6;
					$counters['create_ms_sum'] += $ms;
					$counters['create_max_ms'] = max($counters['create_max_ms'], $ms);
				}
			}
		}
	}

	$now = hrtime(true);
	foreach($peers as $id => &$p){
		try{
			$state = $p['pc']->getState();
		}catch(\Throwable $ex){
			countException($counters, $ex);
			unset($p);
			closePeer($peers, $counters, $id, true);
			continue;
		}
		if(!$p['answered']){
			if($p['pc']->getGatheringState() === GatheringState::COMPLETE){
				$sdp = $p['pc']->getLocalDescription();
				if($sdp !== null && isset($conns[$p['conn']])){
					fwrite($conns[$p['conn']]['stream'], "ANSWER {$p['cid']} " . bin2hex($sdp) . "\n");
					$p['answered'] = true; ++$counters['answers']; $work = true;
				}
			}
		}
		if($state === ConnectionState::CLOSED || $state === ConnectionState::FAILED || $state === ConnectionState::DISCONNECTED){
			$bad = $state !== ConnectionState::CLOSED;
			unset($p);
			closePeer($peers, $counters, $id, $bad);
			continue;
		}
		if($state === ConnectionState::CONNECTED && !$p['ready']){ $p['ready'] = true; ++$counters['connected']; }
		try{
			foreach($p['pc']->pollDataChannels() as $ch){
				$label = $ch->getLabel();
				if($label !== 'ReliableDataChannel' && $label !== 'UnreliableDataChannel'){ ++$counters['bad_channels']; $ch->close(); continue; }
				$p['ch'][$label] = $ch;
				$work = true;
			}
		}catch(\Throwable $ex){ ++$counters['poll_exceptions']; countException($counters, $ex); }
		foreach($p['ch'] as $label => $ch){
			try{
				for($i = 0; $i < 256; ++$i){
					$m = $ch->receive();
					if($m === null){ break; }
					$work = true;
					handleMessage($p, $label, $ch, $m, $counters, $now);
				}
			}catch(\Throwable $ex){ ++$counters['receive_exceptions']; countException($counters, $ex); }
		}
		$rel = $p['ch']['ReliableDataChannel'] ?? null;
		$unrel = $p['ch']['UnreliableDataChannel'] ?? null;
		if($p['flood'] !== null && $rel !== null){
			$fl = &$p['flood'];
			for($i = 0; $i < 64 && ($fl['left'] !== 0); ++$i){
				$msg = 'F' . pack('P', $fl['seq']);
				$msg = str_pad($msg, $fl['size'], 'f');
				try{
					$r = $rel->send($msg);
				}catch(RtcException $ex){
					if($ex->getCode() === RtcException::BUFFER_BUDGET){ ++$counters['flood_budget']; }else{ countException($counters, $ex); $fl['left'] = 0; }
					break;
				}catch(\Throwable $ex){ countException($counters, $ex); $fl['left'] = 0; break; }
				if($r === false){ ++$counters['flood_rejected']; break; }
				++$fl['seq']; ++$counters['flood_sent']; $counters['flood_bytes'] += $fl['size']; $work = true;
				if($fl['left'] > 0 && --$fl['left'] === 0){ ++$counters['flood_done']; }
			}
			unset($fl);
		}
		if($p['stream'] !== null && $rel !== null && $unrel !== null){
			$st = &$p['stream'];
			$burst = 0;
			while($now >= $st['due'] && $burst++ < 32){
				$st['due'] += $st['interval'];
				$msg = str_pad('D' . pack('P', $st['seqRel']), $st['size'], 's');
				try{
					if($rel->send($msg)){ ++$st['seqRel']; ++$counters['stream_sent_rel']; }else{ ++$counters['stream_rejected_rel']; }
				}catch(\Throwable $ex){ countException($counters, $ex); }
				$msg = str_pad('D' . pack('P', $st['seqUnrel']), $st['size'], 's');
				try{
					if($unrel->send($msg)){ ++$counters['stream_sent_unrel']; }else{ ++$counters['stream_rejected_unrel']; }
				}catch(\Throwable $ex){ countException($counters, $ex); }
				++$st['seqUnrel'];
				$work = true;
			}
			if($now - $st['due'] > 1_000_000_000){ $st['due'] = $now; }
			unset($st);
		}
	}
	unset($p);

	$end = hrtime(true);
	$iterMs = ($end - $iterStart) / 1e6;
	++$counters['loop_iters'];
	if($work){ $counters['busy_ns'] += $end - $iterStart; }
	if($iterMs > $counters['loop_max_ms']){ $counters['loop_max_ms'] = round($iterMs, 3); }
	if($iterMs > 10){ ++$counters['loop_over_10ms']; }
	if($iterMs > 100){ ++$counters['loop_over_100ms']; }

	if($sampleFh && ($end - $lastSample) / 1e9 >= $cfg['interval']){
		$lastSample = $end;
		fwrite($sampleFh, json_encode(snapshot($counters, $peers, $start)) . "\n");
		fflush($sampleFh);
	}
	if($quit){ break; }
	if(!$work){ usleep($cfg['idleUs']); }
}

foreach(array_keys($peers) as $id){ closePeer($peers, $counters, $id, false); }
// loop variables keep the last native wrappers alive and would show up as a leak
unset($pc, $ch, $rel, $unrel, $m, $ex, $sdp);
usleep(500000);
$final = snapshot($counters, $peers, $start);
// after every peer is gone, how much of the resident set the allocator hands back when asked
if(($ffi = heapFfi()) !== null){
	$before = procStats();
	$ffi->malloc_trim(0);
	$after = procStats();
	$final['trim'] = ['rss_before_kb' => $before['rss_kb'], 'rss_after_kb' => $after['rss_kb'], 'heap_used_kb' => $after['heap_used_kb'] ?? null];
}
if($sampleFh){ fwrite($sampleFh, json_encode($final) . "\n"); fclose($sampleFh); }
fwrite(STDOUT, 'FINAL ' . json_encode($final) . "\n");
