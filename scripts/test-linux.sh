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
root="$(cd "$(dirname "$0")/.." && pwd -P)"
php_bin="$1"
build_dir="$2"
module="$build_dir/extension/modules/rtc.so"
cd "$root"
"$php_bin" -n scripts/quality.php
"$php_bin" -n tests/contract/check.php
expected="$(find tests -maxdepth 1 -type f -name '*.phpt' -print | wc -l | tr -d '[:space:]')"
((expected > 0)) || { echo 'rtc tests: no PHPT files found' >&2; exit 1; }
mkdir -p "$build_dir/logs"
native_dir="$build_dir/native-tests"
cmake -S "$root/tests/native" -B "$native_dir" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="${RTC_TEST_BACKEND_PREFIX:-$build_dir/install}"
cmake --build "$native_dir" --parallel 2
export RTC_TEST_MODULE="$module"
export RTC_TEST_OFFERER="$native_dir/rtc-offerer"
ctest --test-dir "$native_dir" --output-on-failure
phpt_log="$build_dir/logs/phpt.log"
runner="$build_dir/run-tests.php"
if [[ ! -f "$runner" ]]; then
	php_bin_dir="$(dirname "$(realpath "$php_bin")")"
	candidate="$(realpath -m "$php_bin_dir/../../run-tests.php")"
	[[ -f "$candidate" ]] || { echo "rtc tests: matching run-tests.php missing for $php_bin" >&2; exit 1; }
	cp "$candidate" "$runner"
fi
if NO_INTERACTION=1 REPORT_EXIT_STATUS=1 TEST_PHP_EXECUTABLE="$php_bin" \
	"$php_bin" -n "$runner" -n -d "extension=$module" tests/*.phpt 2>&1 | tee "$phpt_log"; then
	:
else
	statuses=("${PIPESTATUS[@]}")
	php_status=${statuses[0]:-0}
	tee_status=${statuses[1]:-0}
	if ((php_status != 0)); then
		echo "rtc tests: PHPT runner exited with status $php_status" >&2
		exit "$php_status"
	fi
	echo "rtc tests: could not write PHPT log with tee (status $tee_status)" >&2
	exit "$tee_status"
fi
"$php_bin" -n scripts/assert-phpt-summary.php "$expected" "$phpt_log"
bash "$root/scripts/test-teardown-linux.sh" "$php_bin" "$build_dir"
"$php_bin" -n scripts/smoke.php "$module"
