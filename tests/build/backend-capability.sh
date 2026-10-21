#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd -P)"
php_config="$1"
phpize_bin="$2"
patched="$3"
stock="$4"
dir="$5"
[[ "$dir/" == "$root/.build/"* ]] || { echo 'test output must be private' >&2; exit 1; }
mkdir -p "$dir/extension/src" "$dir/mismatched"
ln -s "$patched/include" "$dir/mismatched/include"
ln -s "$stock/lib" "$dir/mismatched/lib"
cp "$root/config.m4" "$root/php_rtc.h" "$dir/extension/"
cp -R "$root/src/." "$dir/extension/src/"
cd "$dir/extension"
"$phpize_bin" > "$dir/phpize.log" 2>&1
for backend in "$stock" "$dir/mismatched"; do
	name=stock
	if [[ "$backend" == "$dir/mismatched" ]]; then name=mismatched; fi
	if ./configure --enable-rtc --with-libdatachannel="$backend" --with-php-config="$php_config" > "$dir/$name.log" 2>&1; then
		echo "FAIL: $name backend was accepted" >&2
		exit 1
	fi
	grep -Fq 'rtc requires linkable libdatachannel 0.24.6 with its message-lifetime overload and identity symbol' "$dir/$name.log"
	cp config.log "$dir/$name-config.log"
	printf 'PASS: %s backend rejected by real compile/link check\n' "$name"
done
