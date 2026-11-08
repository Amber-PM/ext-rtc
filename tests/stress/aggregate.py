#!/usr/bin/env python3
"""median table over the repeated runs of one results directory: aggregate.py DIR"""
import glob, json, os, statistics, sys

d = sys.argv[1]
groups = {}
for f in sorted(glob.glob(os.path.join(d, '*.run*.json'))):
    base = os.path.basename(f)
    if base.count('.') != 2:
        continue
    r = json.load(open(f))
    groups.setdefault(r['scenario'], []).append(r)


def med(v):
    v = [x for x in v if x is not None]
    return round(statistics.median(v), 1) if v else None


print('| scenario | runs | connect ok/attempted | conn p50/p99 ms | srv CPU% steady | RSS peak MB | rel RTT p50/p95/p99 ms | unrel p99 ms | echo/s | unrel loss% | native rejects | end peers/managed | drain s | failed runs |')
print('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
for name, rs in groups.items():
    failed = sum(1 for r in rs if r.get('verdict') == 'FAIL')
    rs = [r for r in rs if 'error' not in r and 'connect_ms' in r]
    if not rs:
        continue
    clean = [r for r in rs if r['connect_fail'] == 0] or rs
    ok = sum(r['connect_ok'] for r in rs)
    tot = sum(r['connect_ok'] + r['connect_fail'] for r in rs)
    s = lambda k: [r['server'].get(k) for r in clean]
    print('| {} | {} | {}/{} | {}/{} | {} | {} | {}/{}/{} | {} | {} | {} | {} | {}/{} | {} | {} |'.format(
        name, len(rs), ok, tot,
        med([r['connect_ms'].get('p50') for r in clean]), med([r['connect_ms'].get('p99') for r in clean]),
        med(s('cpu_pct_steady')), med(s('rss_peak_mb')),
        med([r['rtt_rel'].get('p50_us') / 1000 for r in clean]), med([r['rtt_rel'].get('p95_us') / 1000 for r in clean]), med([r['rtt_rel'].get('p99_us') / 1000 for r in clean]),
        med([r['rtt_unrel'].get('p99_us') / 1000 for r in clean]), med(s('echo_per_s')), med([r['unrel_loss_pct'] for r in clean]),
        sum((r['server'].get('send_rejected_native') or 0) for r in rs), max((r['server'].get('final_active_peers') or 0) for r in rs),
        max((r['server'].get('final_managed_bytes') or 0) for r in rs), max((r.get('drain_s') or 0) for r in rs), failed))
