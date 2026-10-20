#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd -P)"
php_bin="$1"
php_config="$2"
phpize_bin="$3"
prefix="$4"
identity="$("$php_bin" -n -r 'echo PHP_VERSION,"-",PHP_ZTS ? "zts" : "nts";')"
dir="$root/.build/negative-config-$identity"
mkdir -p "$dir/headers-only" "$dir/extension/src"
if [[ ! -e "$dir/headers-only/include" ]]; then ln -s "$prefix/include" "$dir/headers-only/include"; fi
[[ "$(readlink -f "$dir/headers-only/include")" = "$(readlink -f "$prefix/include")" ]]
cp "$root/config.m4" "$root/php_rtc.h" "$dir/extension/"
cp "$root"/src/*.cpp "$root"/src/*.h "$dir/extension/src/"
cd "$dir/extension"
"$phpize_bin" > "$dir/phpize.log" 2>&1
set +e
LIBRARY_PATH="$prefix/lib" ./configure --enable-rtc --with-libdatachannel="$dir/headers-only" --with-php-config="$php_config" > "$dir/configure.log" 2>&1
code=$?
set -e
if [[ "$code" -eq 0 ]]; then echo 'FAIL: missing backend library was accepted' >&2; exit 1; fi
grep -q 'rtc requires linkable libdatachannel 0.24.6 with its exported probe symbol' "$dir/configure.log"
echo "PASS: headers-only backend rejected (configure exit=$code)"
