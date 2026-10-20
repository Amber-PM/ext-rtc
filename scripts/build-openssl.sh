#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd -P)"
source_dir="$root/.build/openssl-source"
prefix="$root/.build/openssl"
commit=67b5686b4419b4cb8caa502711c41815f5279751
if [[ ! -d "$source_dir/.git" ]]; then git clone --branch openssl-3.5.5 https://github.com/openssl/openssl.git "$source_dir"; fi
git -C "$source_dir" diff --quiet && git -C "$source_dir" diff --cached --quiet
git -C "$source_dir" checkout --detach "$commit"
[[ "$(git -C "$source_dir" rev-parse HEAD)" = "$commit" ]]
cd "$source_dir"
./Configure "--prefix=$prefix" "--libdir=lib" shared no-tests
make -j2
make install_sw
LD_LIBRARY_PATH="$prefix/lib" ldd "$prefix/bin/openssl" > "$root/.build/openssl-runtime.txt"
grep -F "$prefix/lib/libssl.so.3" "$root/.build/openssl-runtime.txt"
grep -F "$prefix/lib/libcrypto.so.3" "$root/.build/openssl-runtime.txt"
LD_LIBRARY_PATH="$prefix/lib" "$prefix/bin/openssl" version
