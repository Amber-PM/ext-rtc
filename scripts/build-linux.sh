#!/usr/bin/env bash
set -euo pipefail

fail() { echo "rtc build: $*" >&2; exit 1; }
root="$(cd "$(dirname "$0")/.." && pwd -P)"
php_bin= php_config= phpize_bin= build_dir=
while (($#)); do
	(($# >= 2)) || fail "missing argument value"
	case "$1" in
		--php) php_bin="$2"; shift 2 ;;
		--php-config) php_config="$2"; shift 2 ;;
		--phpize) phpize_bin="$2"; shift 2 ;;
		--build-dir) build_dir="$2"; shift 2 ;;
		*) fail "unknown argument $1" ;;
	esac
done
[[ -n "$php_bin" && -n "$php_config" && -n "$phpize_bin" && -n "$build_dir" ]] || fail "provide --php, --php-config, --phpize and --build-dir"
for tool in "$php_bin" "$php_config" "$phpize_bin" git cmake ninja make c++ pkg-config readelf sha256sum; do
	command -v "$tool" >/dev/null || fail "missing tool $tool"
done
[[ "$build_dir" = /* ]] || fail "build directory must be absolute"
build_dir="$(realpath -m "$build_dir")"
case "$build_dir/" in "$root/.build/"?*/) ;; *) fail "build directory must be a child of $root/.build" ;; esac
[[ "$("$php_bin" -n -r 'echo PHP_VERSION;')" = "$("$php_config" --version)" ]] || fail "PHP and php-config versions differ"
"$php_bin" -n -r 'exit(PHP_VERSION_ID >= 80100 && PHP_INT_SIZE === 8 && !PHP_DEBUG ? 0 : 1);' || fail "requires release PHP 8.1+ x64"
php_api="$("$php_bin" -n -i | awk -F ' => ' '$1 == "PHP API" {print $2}')"
[[ "$php_api" =~ ^[0-9]+$ ]] || fail "cannot inspect PHP API"
"$phpize_bin" --version | grep -q "$php_api" || fail "phpize and PHP APIs differ"
configured_php="$("$php_config" --php-binary)"
[[ "$("$php_bin" -n -r 'echo PHP_VERSION, ":", PHP_ZTS, ":", PHP_DEBUG, ":", PHP_INT_SIZE;')" = "$("$configured_php" -n -r 'echo PHP_VERSION, ":", PHP_ZTS, ":", PHP_DEBUG, ":", PHP_INT_SIZE;')" ]] || fail "php-config binary and requested PHP ABI differ"
lock="$root/build/dependencies.lock.json"
read_lock() { "$php_bin" -n -r '$j=json_decode(file_get_contents($argv[1]),true,512,JSON_THROW_ON_ERROR); foreach(explode(".",$argv[2]) as $k){$j=$j[$k];} echo $j;' "$lock" "$1"; }
commit="$(read_lock libdatachannel.commit)"
ssl_version="$(read_lock openssl.version)"
[[ "$(pkg-config --modversion openssl)" = "$ssl_version" ]] || fail "OpenSSL $ssl_version development library required"
ssl_prefix="$(pkg-config --variable=prefix openssl)"
mkdir -p "$build_dir/logs" "$build_dir/extension/src"
prefix="$build_dir/install"
source_dir="$build_dir/libdatachannel"
if [[ ! -d "$source_dir/.git" ]]; then
	git clone --branch "$(read_lock libdatachannel.tag)" "$(read_lock libdatachannel.repository)" "$source_dir"
fi
git -C "$source_dir" diff --quiet && git -C "$source_dir" diff --cached --quiet || fail "dependency sources have local changes"
git -C "$source_dir" checkout --detach "$commit"
[[ "$(git -C "$source_dir" rev-parse HEAD)" = "$commit" ]] || fail "backend revision mismatch"
git -C "$source_dir" submodule update --init --recursive
"$php_bin" -n -r '
$j=json_decode(file_get_contents($argv[1]),true,512,JSON_THROW_ON_ERROR);
foreach($j["libdatachannel"]["submodules"] as $path=>$sha){
	$out=[]; exec("git -C ".escapeshellarg($argv[2]."/".$path)." rev-parse HEAD",$out,$code);
	if($code!==0 || ($out[0]??null)!==$sha){fwrite(STDERR,"submodule revision mismatch: $path\n");exit(1);}
}' "$lock" "$source_dir"
if git -C "$source_dir" submodule foreach --recursive 'git diff --quiet && git diff --cached --quiet' >/dev/null; then :; else fail "modified submodule"; fi
cmake -S "$source_dir" -B "$build_dir/backend" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
	-DCMAKE_C_FLAGS=-DICE_MAX_CANDIDATES_COUNT=64 \
	-DOPENSSL_ROOT_DIR="$ssl_prefix" -DOPENSSL_USE_STATIC_LIBS=OFF \
	-DCMAKE_INSTALL_RPATH="$prefix/lib;$ssl_prefix/lib;$ssl_prefix/lib64" \
	-DBUILD_SHARED_LIBS=ON -DBUILD_SHARED_DEPS_LIBS=OFF \
	-DNO_MEDIA=ON -DNO_WEBSOCKET=ON -DNO_EXAMPLES=ON -DNO_TESTS=ON \
	-DUSE_GNUTLS=OFF -DUSE_MBEDTLS=OFF -DUSE_NICE=OFF \
	-DUSE_SYSTEM_JUICE=OFF -DUSE_SYSTEM_USRSCTP=OFF -DUSE_SYSTEM_PLOG=OFF
cmake --build "$build_dir/backend" --parallel 2
cmake --install "$build_dir/backend"
cp "$root/config.m4" "$root/php_rtc.h" "$build_dir/extension/"
cp "$root"/src/*.cpp "$root"/src/*.h "$build_dir/extension/src/"
(
	cd "$build_dir/extension"
	"$phpize_bin"
	./configure --enable-rtc --with-libdatachannel="$prefix" --with-php-config="$php_config"
	make -j2
)
module="$build_dir/extension/modules/rtc.so"
"$php_bin" -n -d "extension=$module" --ri rtc
ldd "$module" > "$build_dir/logs/linked-libraries.txt"
resolved_backend="$(awk '$1 == "libdatachannel.so.0.24" {print $3}' "$build_dir/logs/linked-libraries.txt")"
[[ -n "$resolved_backend" && "$(realpath "$resolved_backend")" = "$(realpath "$prefix/lib/libdatachannel.so")" ]] || fail "loader resolved a backend outside the verified prefix"
awk '/=> \// {print $3}' "$build_dir/logs/linked-libraries.txt" | while IFS= read -r library; do sha256sum "$library"; done > "$build_dir/logs/linked-library-sha256.txt"
readelf -d "$module" > "$build_dir/logs/dynamic-section.txt"
sha256sum "$module" "$prefix"/lib/libdatachannel.so.* > "$build_dir/logs/sha256.txt"
"$php_bin" -n -r 'echo json_encode(["php"=>PHP_VERSION,"api"=>(int)$argv[1],"zts"=>PHP_ZTS,"debug"=>PHP_DEBUG,"bits"=>PHP_INT_SIZE*8],JSON_PRETTY_PRINT),"\n";' "$php_api" > "$build_dir/logs/php-abi.json"
pkg-config --modversion openssl > "$build_dir/logs/openssl-version.txt"
git -C "$source_dir" submodule status --recursive > "$build_dir/logs/submodules.txt"
mkdir -p "$prefix/share/rtc/licenses"
cp "$root"/third-party/licenses/*.txt "$prefix/share/rtc/licenses/"
cp "$root/LICENSE" "$prefix/share/rtc/ext-rtc-LICENSE"
cp "$lock" "$prefix/share/rtc/dependencies.lock.json"
cp "$root/third-party/NOTICES.md" "$prefix/share/rtc/NOTICES.md"
runner="$("$php_config" --prefix)/lib/php/build/run-tests.php"
if [[ ! -f "$runner" ]]; then runner="/usr/lib/php/$php_api/build/run-tests.php"; fi
[[ -f "$runner" ]] || fail "matching PHP run-tests.php missing"
cp "$runner" "$build_dir/run-tests.php"
echo "rtc build complete: $module"
