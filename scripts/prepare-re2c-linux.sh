#!/usr/bin/env bash
#
#  _____ __  _______   ____ _____ ____
# | ____|\ \/ /_   _| |  _ \_   _/ ___|
# |  _|   \  /  | |   | |_) || || |
# | |___  /  \  | |   |  _ < | || |___
# |_____|/_/\_\ |_|   |_| \_\|_| \____|
#
# Copyright (c) 2026 Amber-PM contributors
# SPDX-License-Identifier: MIT
#
# https://github.com/Amber-PM/ext-rtc
#
set -euo pipefail

fail() { echo "rtc re2c: $*" >&2; exit 1; }
root="$(cd "$(dirname "$0")/.." && pwd -P)"
lock="$root/build/dependencies.lock.json"
build_dir="$root/.build/re2c"
read_lock() {
	python3 - "$lock" "buildTools.linux.re2c.$1" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as lock_file:
    value = json.load(lock_file)
for key in sys.argv[2].split("."):
    value = value[key]
print(value)
PY
}
archive="$(read_lock archive)"
url="$(read_lock url)"
expected_sha="$(read_lock sha256)"
expected_version="$(read_lock version)"
expected_package_version="$(read_lock packageVersion)"
expected_arch="$(read_lock architecture)"

[[ "$(dpkg --print-architecture)" = "$expected_arch" ]] || fail "requires $expected_arch Ubuntu runner"
[[ "$archive" != */* ]] || fail "locked package archive must be a filename"
for tool in curl dpkg-deb python3 sha256sum; do
	command -v "$tool" >/dev/null || fail "missing tool $tool"
done
mkdir -p "$build_dir"
package="$build_dir/$archive"
if [[ ! -f "$package" ]]; then
	curl --fail --location --retry 3 --output "$package" "$url"
fi
echo "$expected_sha  $package" | sha256sum --check || fail "locked re2c package SHA-256 mismatch"
[[ "$(dpkg-deb -f "$package" Package)" = re2c ]] || fail "unexpected package name"
[[ "$(dpkg-deb -f "$package" Version)" = "$expected_package_version" ]] || fail "unexpected package version"
[[ "$(dpkg-deb -f "$package" Architecture)" = "$expected_arch" ]] || fail "unexpected package architecture"

package_root="$build_dir/root"
re2c="$package_root/usr/bin/re2c"
if [[ ! -x "$re2c" ]]; then
	[[ ! -e "$package_root" ]] || fail "partial re2c package extraction exists"
	mkdir -p "$package_root"
	dpkg-deb --extract "$package" "$package_root"
fi
actual_version="$("$re2c" --version | awk '{print $2}')"
[[ "$actual_version" = "$expected_version" ]] || fail "expected re2c $expected_version, found $actual_version"
printf 're2c %s from %s\n' "$actual_version" "$archive"
