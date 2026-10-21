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
require __DIR__.'/../tests/support/network.php';

[$module,$probe]=array_slice($argv,1,2);
$socket=stream_socket_server('tcp://127.0.0.1:0',$errno,$error);
check(is_resource($socket),'test socket: '.$error);
$address=stream_socket_get_name($socket,false);fclose($socket);
[$process,$pipes]=startTestProcess([PHP_BINARY,'-n','-d','extension='.$module,'-d','extension='.$probe,
	'-d','display_errors=0','-d','log_errors=1','-S',$address,__DIR__.'/../tests/support/bailout-server.php']);
fclose($pipes[0]);
$request=static function(array $query)use($address):string|false{
	$context=stream_context_create(['http'=>['ignore_errors'=>true,'timeout'=>20]]);
	return @file_get_contents('http://'.$address.'/?'.http_build_query($query),false,$context);
};
try{
	$baseline=null;
	waitFor(function()use($request,&$baseline){$body=$request([]);if($body===false){return false;}$baseline=json_decode($body,true);return is_array($baseline);});
	$verify=static function()use($request,$baseline):void{
		$next=json_decode($request([]),true,512,JSON_THROW_ON_ERROR);
		check($next['pid']===$baseline['pid'],'persistent process was replaced');
		check($next['stats']['activePeers']===0 && $next['stats']['activeChannels']===0,'request transports leaked');
		check($next['stats']['managedBufferBytes']===$baseline['stats']['managedBufferBytes'],'native owners leaked across requests: '.json_encode($next));
	};
	$request(['mode'=>'memory']);$verify();
	$tests=0;
	foreach(['getLocalDescription','getRemoteFingerprint','getLocalAddress','getRemoteAddress','getStats','pollLocalCandidates'] as $method){
		$normal=json_decode($request(['mode'=>'call','method'=>$method]),true,512,JSON_THROW_ON_ERROR);
		check($normal['allocations']>0,'probe missed '.$method);$verify();
		for($at=1;$at<=$normal['allocations'];++$at){
			$body=$request(['mode'=>'call','method'=>$method,'at'=>$at]);
			check($body!==false && !str_contains($body,'allocations'),'bailout missing: '.$method.' allocation '.$at);
			$verify();++$tests;
		}
		echo $method.' allocations='.$normal['allocations']." clean\n";
	}
	echo 'PASS '.$tests." injected allocation bailouts, normal and memory-fatal requests in one process\n";
}finally{
	proc_terminate($process);
	waitFor(fn()=> !proc_get_status($process)['running']);
	$error=processOutput($pipes[2]);
	fclose($pipes[1]);fclose($pipes[2]);proc_close($process);
	check(str_contains($error,'Allowed memory size'),'real memory fatal missing');
	check(str_contains($error,'injected Zend allocation bailout'),'injection not reached');
}
