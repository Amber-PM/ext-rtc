#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""latency limits of the stress gate, derived from recorded baseline runs

usage:
  calibrate.py export OUT.json RESULTS_DIR [RESULTS_DIR ...]   record the runs of a known good module
  calibrate.py table [BASELINE.json]                           print LATENCY_LIMITS_MS and FAST_FREEZE_P99_LIMIT_MS for stress.py
  calibrate.py check [BASELINE.json]                           fail when the tables of stress.py differ from the recorded runs,
                                                               when a scenario of run-all.sh has no limit or fewer than three runs

A limit is MARGIN times the worst recorded value of the scenario. MARGIN is defined in stress.py.
"""
import glob
import importlib.util
import json
import math
import os
import re
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT = os.path.join(HERE, 'latency-baseline.json')
MIN_RUNS = 3


def load_stress():
    spec = importlib.util.spec_from_file_location('stress', os.path.join(HERE, 'stress.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def export(out, dirs):
    runs = defaultdict(list)
    for d in dirs:
        for path in sorted(glob.glob(os.path.join(d, '*.run*.json'))):
            name = os.path.basename(path)
            if name.count('.') != 2:
                continue
            row = json.load(open(path))
            if 'error' in row or 'rtt_rel' not in row or row.get('verdict') == 'FAIL':
                continue
            scenario = re.sub(r'\.run\d+\.json$', '', name)
            entry = {'p95_ms': round(row['rtt_rel'].get('p95_us', 0) / 1000.0, 3), 'p99_ms': round(row['rtt_rel'].get('p99_us', 0) / 1000.0, 3),
                     'load_before': row.get('loadavg_before', [None])[0]}
            fast = row.get('fast_during_freeze')
            if fast:
                entry['fast_p99_ms'] = round(fast.get('p99_us_max', 0) / 1000.0, 3)
            runs[scenario].append(entry)
    json.dump({'about': 'reliable echo round trips of the original module (e57c534) on an idle machine, input of calibrate.py', 'runs': runs},
              open(out, 'w'), indent=1, sort_keys=True)
    open(out, 'a').write('\n')
    print('recorded', sum(len(v) for v in runs.values()), 'runs of', len(runs), 'scenarios in', out)
    return 0


def derive(baseline, margin):
    limits, freeze = {}, {}
    for scenario, rows in sorted(baseline['runs'].items()):
        limits[scenario] = (math.ceil(max(r['p95_ms'] for r in rows) * margin), math.ceil(max(r['p99_ms'] for r in rows) * margin))
        fast = [r['fast_p99_ms'] for r in rows if 'fast_p99_ms' in r]
        if fast:
            freeze[scenario] = math.ceil(max(fast) * margin)
    return limits, freeze


def table(path):
    stress = load_stress()
    limits, freeze = derive(json.load(open(path)), stress.LATENCY_MARGIN)
    print('LATENCY_LIMITS_MS = {')
    for scenario, pair in limits.items():
        print(f"    {scenario!r}: {pair},")
    print('}')
    print('FAST_FREEZE_P99_LIMIT_MS = ' + repr(freeze))
    return 0


def check(path):
    stress = load_stress()
    baseline = json.load(open(path))
    limits, freeze = derive(baseline, stress.LATENCY_MARGIN)
    problems = []
    for scenario, pair in limits.items():
        runs = len(baseline['runs'][scenario])
        if runs < MIN_RUNS:
            problems.append(f'{scenario}: {runs} recorded runs, at least {MIN_RUNS} are needed')
        if stress.LATENCY_LIMITS_MS.get(scenario) != pair:
            problems.append(f'{scenario}: stress.py has {stress.LATENCY_LIMITS_MS.get(scenario)}, the recorded runs give {pair}')
    for scenario in stress.LATENCY_LIMITS_MS:
        if scenario not in limits:
            problems.append(f'{scenario}: limit without recorded runs')
    if dict(stress.FAST_FREEZE_P99_LIMIT_MS) != freeze:
        problems.append(f'FAST_FREEZE_P99_LIMIT_MS is {stress.FAST_FREEZE_P99_LIMIT_MS}, the recorded runs give {freeze}')
    script = open(os.path.join(HERE, 'run-all.sh')).read()
    for scenario in sorted(set(re.findall(r'\b(?:connect|reconnect|both|abrupt|slow|slowdelay|soak)-\d+-(?:mux|ports)\b', script))):
        if scenario not in stress.LATENCY_LIMITS_MS:
            problems.append(f'{scenario}: run-all.sh runs it but it has no latency limit')
    for line in problems:
        print('FAIL', line)
    if not problems:
        print(f'PASS latency limits: {len(limits)} scenarios, margin {stress.LATENCY_MARGIN}, every scenario of run-all.sh is covered')
    return 1 if problems else 0


def main():
    if len(sys.argv) >= 4 and sys.argv[1] == 'export':
        return export(sys.argv[2], sys.argv[3:])
    if len(sys.argv) >= 2 and sys.argv[1] in ('table', 'check'):
        path = sys.argv[2] if len(sys.argv) > 2 else DEFAULT
        return table(path) if sys.argv[1] == 'table' else check(path)
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main())
