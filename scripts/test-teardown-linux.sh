#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd -P)"
php_bin="$1"
build_dir="$2"
prefix="${RTC_TEST_BACKEND_PREFIX:-$build_dir/install}"
module="$build_dir/extension/modules/rtc.so"
php_config="$(dirname "$(realpath "$php_bin")")/php-config"
helper="$build_dir/teardown-probe.so"
[[ -x "$php_config" ]] || { echo 'rtc teardown tests require matching php-config' >&2; exit 1; }
c++ -std=c++20 -shared -fPIC -DRTC_ENABLE_MEDIA=0 -DRTC_ENABLE_WEBSOCKET=0 \
	$("$php_config" --includes) -I"$prefix/include" \
	"$root/tests/native/teardown-php.cpp" "$module" \
	-L"$prefix/lib" -ldatachannel -ldl -pthread -Wl,-rpath,"$prefix/lib:$build_dir/extension/modules" \
	-o "$helper"
RTC_TEARDOWN_LOG="$build_dir/logs/teardown-completion.log" LD_PRELOAD="$helper" \
	"$php_bin" -n -d "extension=$helper" -d "extension=$module" \
	-r 'if(!extension_loaded("rtc")){exit(1);} teardown_probe_start();' > "$build_dir/logs/teardown-request.log" 2>&1
grep -Fq 'MSHUTDOWN budget=0 owners=0 native release after RSHUTDOWN' "$build_dir/logs/teardown-completion.log"
printf '%s\n' 'PASS: actual PHP RSHUTDOWN retained escrow until native Message destruction'

mkdir -p "$build_dir/wrong-backend"
# php deep binding can bypass a standalone symbol override
"$php_bin" -n -r '
$lock=json_decode(file_get_contents($argv[1]),true,512,JSON_THROW_ON_ERROR);
$identity=$lock["libdatachannel"]["identity"];
$library=file_get_contents($argv[2]);
if(substr_count($library,$identity)!==1){fwrite(STDERR,"backend identity fixture is ambiguous\n");exit(1);}
$library=str_replace($identity,"X".substr($identity,1),$library);
if(file_put_contents($argv[3],$library)!==strlen($library)){exit(1);}
' "$root/build/dependencies.lock.json" "$prefix/lib/libdatachannel.so" "$build_dir/wrong-backend/libdatachannel.so.0.24"
if LD_PRELOAD="$build_dir/wrong-backend/libdatachannel.so.0.24" "$php_bin" -n -d "extension=$module" \
	-r 'exit(extension_loaded("rtc") ? 0 : 1);' > "$build_dir/logs/wrong-backend-identity.log" 2>&1; then
	echo 'rtc loaded despite incompatible backend identity' >&2
	exit 1
fi
grep -Fq 'libdatachannel identity or message lifetime linkage probe failed' "$build_dir/logs/wrong-backend-identity.log"
printf '%s\n' 'PASS: incompatible loaded backend fails module initialization'
