#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd -P)"
version="$1"
mode="$2"
case "$version" in
	8.4.27) sha=dcf93923b0e38e0d5252b25b796520b5a47761a08b3488b6d0b5d1115764d3f0 ;;
	8.5.12) sha=08bc2eba2a588654ade1357174aa646b761556c862dda0678345ee29ed481209 ;;
	*) echo "Unpinned PHP version" >&2; exit 1 ;;
esac
case "$mode" in nts) zts=--disable-zts ;; zts) zts=--enable-zts ;; *) echo "Choose nts or zts" >&2; exit 1 ;; esac
dir="$root/.build/php-$version-$mode"
# built-in openssl matches common php builds and exposes backend exit races
ssl="$root/.build/openssl"
[[ -f "$ssl/lib/pkgconfig/openssl.pc" ]] || { echo "Run scripts/build-openssl.sh first" >&2; exit 1; }
mkdir -p "$dir"
archive="$root/.build/php-$version.tar.xz"
if [[ ! -f "$archive" ]]; then curl --fail --location --output "$archive" "https://www.php.net/distributions/php-$version.tar.xz"; fi
echo "$sha  $archive" | sha256sum --check
if [[ ! -d "$dir/source" ]]; then mkdir "$dir/source"; tar -xJf "$archive" -C "$dir/source" --strip-components=1; fi
cd "$dir/source"
PKG_CONFIG_PATH="$ssl/lib/pkgconfig" LDFLAGS="-Wl,-rpath,$ssl/lib" ./configure --prefix="$dir/install" --disable-all --enable-cli --disable-cgi --disable-phpdbg --without-pear --with-openssl "$zts" > "$dir/configure.log" 2>&1
make -j2 > "$dir/make.log" 2>&1
make install > "$dir/install.log" 2>&1
cp run-tests.php "$dir/run-tests.php"
"$dir/install/bin/php" -n -r 'exit(extension_loaded("openssl") ? 0 : 1);' || { echo "PHP was built without openssl" >&2; exit 1; }
ldd "$dir/install/bin/php" | grep -F "$ssl/lib/libcrypto.so.3" || { echo "PHP does not resolve the locked libcrypto" >&2; exit 1; }
"$dir/install/bin/php" -n -v
