#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd -P)"
build_dir="$(realpath "$1")"
[[ "$build_dir/" == "$root/.build/"* ]] || { echo 'sanitizer output must be private' >&2; exit 1; }
prefix="$build_dir/sanitized-install"
ssl_prefix="$(pkg-config --variable=prefix openssl)"
cmake -S "$build_dir/backend-source" -B "$build_dir/sanitized-backend" -G Ninja \
	-DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
	'-DCMAKE_C_FLAGS=-DICE_MAX_CANDIDATES_COUNT=64 -fsanitize=address,undefined -fno-omit-frame-pointer' \
	'-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
	-DOPENSSL_ROOT_DIR="$ssl_prefix" -DOPENSSL_USE_STATIC_LIBS=OFF \
	-DCMAKE_INSTALL_RPATH="$prefix/lib;$ssl_prefix/lib;$ssl_prefix/lib64" \
	-DBUILD_SHARED_LIBS=ON -DBUILD_SHARED_DEPS_LIBS=OFF -DNO_MEDIA=ON -DNO_WEBSOCKET=ON \
	-DNO_EXAMPLES=ON -DNO_TESTS=ON -DUSE_GNUTLS=OFF -DUSE_MBEDTLS=OFF -DUSE_NICE=OFF \
	-DUSE_SYSTEM_JUICE=OFF -DUSE_SYSTEM_USRSCTP=OFF -DUSE_SYSTEM_PLOG=OFF
cmake --build "$build_dir/sanitized-backend" -j2
cmake --install "$build_dir/sanitized-backend"
cmake -S "$root/tests/native" -B "$build_dir/sanitized-native" -DCMAKE_BUILD_TYPE=Debug \
	-DCMAKE_PREFIX_PATH="$prefix" '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer'
cmake --build "$build_dir/sanitized-native" -j2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	ctest --test-dir "$build_dir/sanitized-native" --output-on-failure
