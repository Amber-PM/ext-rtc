--TEST--
rtc reflection matches the frozen stub signatures
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
$stub=file_get_contents(__DIR__.'/../stubs/rtc.stub.php');
$count=0;
$codes=['INVALID_ARGUMENT','CLOSED','INVALID_STATE','INVALID_SDP','SDP_TOO_LARGE','PAYLOAD_TOO_LARGE','CANDIDATE_LIMIT','CHANNEL_LIMIT','RECEIVE_OVERFLOW','BUFFER_BUDGET','BACKEND_FAILURE','SHUTDOWN_FAILURE'];
foreach($codes as $i=>$code){ if(constant('amber\\rtc\\RtcException::'.$code)!==$i+1){ throw new RuntimeException('exception code'); } }
if(array_map(fn($c)=>$c->name,amber\rtc\ConnectionState::cases())!==['NEW','CONNECTING','CONNECTED','DISCONNECTED','FAILED','CLOSED']){ throw new RuntimeException('connection states'); }
if(array_map(fn($c)=>$c->name,amber\rtc\GatheringState::cases())!==['NEW','IN_PROGRESS','COMPLETE']){ throw new RuntimeException('gathering states'); }
foreach(['PeerConnectionOptions','IceServer','IceCandidate','PeerConnection'] as $class){
	preg_match('/final class '.$class.'\{(.*?)\n\}/s',$stub,$block);
	preg_match_all('/public (static )?function (\w+)\((.*?)\)(?: : ([?\w]+))?/',$block[1],$methods,PREG_SET_ORDER);
	foreach($methods as $match){
		[, $static,$name,$args]=$match;
		if(in_array($name,['createDataChannel','pollDataChannels'],true)){ continue; }
		$r=new ReflectionMethod('amber\\rtc\\'.$class,$name);
		if(!$r->isInternal() || $r->isStatic()!==($static!=='')){ throw new RuntimeException($class.'::'.$name.' flags'); }
		$return=isset($match[4])?$match[4]:'';
		if((string)$r->getReturnType()!==($return===''?'':(in_array(ltrim($return,'?'),['self','string','int','bool','array','void'],true)?$return:($return[0]==='?'?'?':'').'amber\\rtc\\'.ltrim($return,'?')))){
			throw new RuntimeException($class.'::'.$name.' return');
		}
		$expected=$args===''?[]:explode(', ',$args);
		if(count($r->getParameters())!==count($expected)){ throw new RuntimeException('parameter count'); }
		foreach($r->getParameters() as $i=>$p){
			preg_match('/(\??\w+) (\.\.\.)?\$(\w+)(?: = (.*))?/',$expected[$i],$arg);
			$type=$arg[1];
			if(!in_array(ltrim($type,'?'),['int','bool','string'],true)){ $type=($type[0]==='?'?'?':'').'amber\\rtc\\'.ltrim($type,'?'); }
			if($p->getName()!==$arg[3] || (string)$p->getType()!==$type || $p->isVariadic()!==($arg[2]!=='')){
				throw new RuntimeException($class.'::'.$name.' parameter '.$i);
			}
			if(isset($arg[4])){
				$default=$arg[4]; $expectedDefault=$default==='null'?null:($default[0]==="'"?trim($default,"'"):(int)$default);
				if(!$p->isDefaultValueAvailable() || $p->getDefaultValue()!==$expectedDefault){ throw new RuntimeException('default'); }
			}
		}
		++$count;
	}
}
if($count!==31){ throw new RuntimeException('signature coverage '.$count); }
echo "transport reflection OK\n";
?>
--EXPECT--
transport reflection OK
