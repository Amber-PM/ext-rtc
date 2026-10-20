#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd -P)"
version="$1"
mode="$2"
case "$version" in
	8.1.34) sha=ffa9e0982e82eeaea848f57687b425ed173aa278fe563001310ae2638db5c251 ;;
	8.4.24) sha=e127be09a8506f4327c5cfa78a614b00d210714484ec215ce0011b4a03c00731 ;;
	*) echo "Unpinned PHP version" >&2; exit 1 ;;
esac
case "$mode" in nts) zts=--disable-zts ;; zts) zts=--enable-zts ;; *) echo "Choose nts or zts" >&2; exit 1 ;; esac
dir="$root/.build/php-$version-$mode"
mkdir -p "$dir"
archive="$root/.build/php-$version.tar.xz"
if [[ ! -f "$archive" ]]; then curl --fail --location --output "$archive" "https://www.php.net/distributions/php-$version.tar.xz"; fi
echo "$sha  $archive" | sha256sum --check
if [[ ! -d "$dir/source" ]]; then mkdir "$dir/source"; tar -xJf "$archive" -C "$dir/source" --strip-components=1; fi
cd "$dir/source"
./configure --prefix="$dir/install" --disable-all --enable-cli --disable-cgi --disable-phpdbg --without-pear "$zts" > "$dir/configure.log" 2>&1
make -j2 > "$dir/make.log" 2>&1
make install > "$dir/install.log" 2>&1
cp run-tests.php "$dir/run-tests.php"
"$dir/install/bin/php" -n -v
