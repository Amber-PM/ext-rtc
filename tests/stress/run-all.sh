#!/usr/bin/env bash
# usage: run-all.sh [--keep-going] MODULE OUTDIR [REPEAT] [extra stress.py options]
# builds the client when missing, then runs the matrix as two groups (OUTDIR/connect and OUTDIR/scenarios)
# exit 0 only when every run of every group passed, 1 when a run failed, 2 on bad usage or a missing prerequisite
# by default the first failing group stops the matrix, --keep-going runs the remaining groups anyway
# client build: RTC_STRESS_CLIENT names a prebuilt client, otherwise it is built into .build/stress against
# RTC_PREFIX (patched libdatachannel install) or the first .build/*/install of this checkout
set -uo pipefail
keep=0
if [[ "${1:-}" == "--keep-going" ]]; then keep=1; shift; fi
if (($# < 2)); then echo "usage: run-all.sh [--keep-going] MODULE OUTDIR [REPEAT] [stress.py options]" >&2; exit 2; fi
module="$1"; out="$2"; repeat="${3:-3}"
[[ "$repeat" =~ ^[1-9][0-9]*$ ]] || { echo "run-all: REPEAT must be a positive integer, got $repeat" >&2; exit 2; }; shift 3 || shift $#
here="$(cd "$(dirname "$0")" && pwd -P)"
root="$(cd "$here/../.." && pwd -P)"
client="${RTC_STRESS_CLIENT:-$root/.build/stress/rtc-stress-client}"
if [[ ! -x "$client" ]]; then
	prefix="${RTC_PREFIX:-}"
	if [[ -z "$prefix" ]]; then
		for candidate in "$root"/.build/*/install; do
			if [[ -f "$candidate/lib/cmake/LibDataChannel/LibDataChannelConfig.cmake" ]]; then prefix="$candidate"; break; fi
		done
	fi
	if [[ -z "$prefix" ]]; then echo "run-all: no client at $client, set RTC_PREFIX to the patched libdatachannel install" >&2; exit 2; fi
	cmake -S "$here" -B "$(dirname "$client")" -DCMAKE_PREFIX_PATH="$prefix" -G Ninja >/dev/null && cmake --build "$(dirname "$client")" || { echo "run-all: client build failed" >&2; exit 2; }
fi
mkdir -p "$out"
echo "nproc=$(nproc) load=$(cut -d' ' -f1-3 /proc/loadavg) module=$module" | tee "$out/env.txt"
py=python3
status=0
run_group() {
	local name="$1" count="$2"; shift 2
	"$py" "$here/stress.py" --module "$module" --client "$client" --out "$out/$name" --repeat "$count" "$@"
	local rc=$?
	echo "group $name: exit $rc" | tee -a "$out/groups.txt"
	if ((rc != 0)); then status=1; fi
	return $rc
}
# 100/300/500 connections in mux and individual-port mode, the other scenarios in mux mode
run_group connect "$repeat" "$@" \
	connect-100-mux connect-300-mux connect-500-mux connect-100-ports connect-300-ports connect-500-ports
if ((status != 0 && keep == 0)); then
	echo "run-all: FAILED, stopping after group connect (use --keep-going to run the rest)" >&2
	exit 1
fi
run_group scenarios 1 "$@" \
	reconnect-100-mux reconnect-300-mux reconnect-100-ports both-100-mux abrupt-100-mux abrupt-100-ports slow-100-mux slowdelay-100-mux soak-100-mux
if ((status != 0)); then echo "run-all: FAILED, see verdict.json in each group directory" >&2; exit 1; fi
echo "run-all: PASS"
