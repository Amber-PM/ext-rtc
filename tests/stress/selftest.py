#!/usr/bin/env python3
"""negative tests of the stress harness

usage: selftest.py --module rtc.so --client rtc-stress-client --out DIR [--stress PATH/stress.py] [--legacy]

Injects faults around a REAL server and client (small runs of connect-10/20) and checks that stress.py exits with the expected code
and names the expected failure in verdict.json. The faults are applied to what the harness sees (a php wrapper that kills, freezes or
rewrites the server, a client wrapper that rewrites the client JSON), so they prove that the harness detects each reported condition,
not that the extension produces it. Exit 0 only when every case behaved as expected, so a harness that hides failures makes this fail.
--legacy runs the same cases against an older stress.py (no new options) to show what it let through.
"""
import argparse, json, os, shutil, stat, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))

PHPFAULT = r'''#!/usr/bin/env python3
import json, os, signal, subprocess, sys, threading, time
mode, php, args = sys.argv[1], sys.argv[2], sys.argv[3:]
samples = next((a.split('=', 1)[1] for a in args if a.startswith('--samples=')), None)
if mode == 'idle40':
    # a server that sleeps 40 ms whenever it has no work answers every echo late, like a starved dispatcher would
    args = args + ['--idle-us=40000']
p = subprocess.Popen([php] + args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
if mode in ('crash', 'hang'):
    def later():
        time.sleep(3)
        os.kill(p.pid, signal.SIGKILL if mode == 'crash' else signal.SIGSTOP)
    threading.Thread(target=later, daemon=True).start()
for raw in p.stdout:
    line = raw.decode('utf-8', 'replace')
    if line.startswith('FINAL '):
        d = json.loads(line[6:])
        if mode == 'leakpeers':
            d['runtime']['activePeers'] = 3
        if mode == 'leakbytes':
            d['runtime']['managedBufferBytes'] = 4096
        if mode == 'overflow':
            d['runtime']['receiveOverflowCount'] = 2
        if mode == 'exc':
            d['counters']['echo_exceptions'] = 1
        line = 'FINAL ' + json.dumps(d) + '\n'
    sys.stdout.write(line)
    sys.stdout.flush()
rc = p.wait()
if samples and os.path.exists(samples) and mode in ('threads', 'fds'):
    rows = [json.loads(l) for l in open(samples) if l.strip()]
    last = dict(rows[-1])
    last['t'] += 1
    last['threads' if mode == 'threads' else 'fds'] += 60
    with open(samples, 'a') as f:
        f.write(json.dumps(last) + '\n')
if samples and os.path.exists(samples) and mode == 'nosamples':
    open(samples, 'w').close()
if samples and os.path.exists(samples) and mode == 'soakgrowth':
    rows = [json.loads(l) for l in open(samples) if l.strip()]
    with open(samples, 'w') as f:
        for r in rows:
            r['fds'] += int(r['t'] * 5)
            f.write(json.dumps(r) + '\n')
sys.exit(7 if mode == 'exit7' else (rc if rc >= 0 else 128 - rc))
'''

CLIENTFAULT = r'''#!/usr/bin/env python3
import json, os, subprocess, sys
mode, real, args = sys.argv[1], sys.argv[2], sys.argv[3:]
if mode == 'novictim' and '--duration' in args and float(args[args.index('--duration') + 1]) >= 3000:
    sys.exit(0)
if mode == 'shortvictim' and '--duration' in args and float(args[args.index('--duration') + 1]) >= 3000:
    # the victims connect, then leave on their own before anything is killed
    args[args.index('--duration') + 1] = '2'
rc = subprocess.call([real] + args)
out = args[args.index('--out') + 1]
if mode == 'crash':
    if os.path.exists(out):
        os.remove(out)
    sys.exit(139)
d = json.load(open(out))
if mode == 'fail1':
    d['connect']['fail'] += 1
elif mode == 'fail5':
    d['connect']['fail'] += 5
elif mode == 'order':
    d['stream']['rel_order_violations'] = 2
elif mode == 'floodorder':
    d['flood']['order_violations'] = 1
elif mode == 'cleanup':
    d['cleanup_ok'] = False
elif mode == 'unrelloss':
    d['echo']['unrel_loss_pct'] = 12.5
elif mode == 'relunanswered':
    d['echo']['rel_recv'] = d['echo']['rel_sent'] // 2
elif mode == 'dropped':
    d['dropped_peers'] = 4
elif mode == 'largefail':
    d['large_message_fail'] = 1
elif mode == 'errors':
    d['errors'] = ['boom']
elif mode == 'norttn':
    if 'rtt_reliable' in d:
        d['rtt_reliable']['n'] = 0
json.dump(d, open(out, 'w'))
sys.exit(3 if mode == 'exitcode' else rc)
'''


def write_exec(path, text):
    with open(path, 'w') as f:
        f.write(text)
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--module', required=True)
    ap.add_argument('--client', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--stress', default=os.path.join(HERE, 'stress.py'))
    ap.add_argument('--legacy', action='store_true', help='older stress.py without the new options: skips cases that need them')
    ap.add_argument('--only', default='', help='comma separated case names')
    args = ap.parse_args()
    out = os.path.abspath(args.out)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    php = shutil.which('php') or 'php'
    write_exec(os.path.join(out, 'phpfault.py'), PHPFAULT)
    write_exec(os.path.join(out, 'clientfault.py'), CLIENTFAULT)

    def php_wrapper(mode):
        path = os.path.join(out, f'php-{mode}.sh')
        write_exec(path, f'#!/bin/sh\nexec python3 "{out}/phpfault.py" {mode} "{php}" "$@"\n')
        return path

    def client_wrapper(mode):
        path = os.path.join(out, f'client-{mode}.sh')
        write_exec(path, f'#!/bin/sh\nexec python3 "{out}/clientfault.py" {mode} "{os.path.abspath(args.client)}" "$@"\n')
        return path

    empty = os.path.join(out, 'broken.so')
    open(empty, 'w').close()
    base = ['--hold', '4', '--stats-timeout', '4', '--quit-timeout', '6'] if not args.legacy else ['--hold', '4']
    # name, stress options, scenario, expected exit, accepted failure texts in verdict.json (None: no check), needs new options
    cases = [
        ('positive-control', [], 'connect-10-mux', 0, None, False),
        ('positive-control-neutral-wrappers', ['--php', php_wrapper('none'), '--client', client_wrapper('none')], 'connect-10-mux', 0, None, False),
        ('module-missing', ['--module', '/nonexistent/rtc.so'], 'connect-10-mux', 2, None, False),
        ('module-broken', ['--module', empty], 'connect-10-mux', 1, ['server_not_ready', 'harness_exception'], False),
        ('scenario-unknown', [], 'bogus-10-mux', 2, None, False),
        ('server-crash', ['--php', php_wrapper('crash'), '--hold', '9'], 'connect-10-mux', 1, ['server_no_final', 'harness_exception', 'server_exit_code', 'dropped'], False),
        ('server-hang', ['--php', php_wrapper('hang'), '--hold', '6'], 'connect-10-mux', 1, ['server_no_final', 'harness_exception', 'server_exit_code', 'not_drained'], False),
        ('server-exit-code', ['--php', php_wrapper('exit7')], 'connect-10-mux', 1, ['server_exit_code'], False),
        ('leaked-peers', ['--php', php_wrapper('leakpeers')], 'connect-10-mux', 1, ['leaked_peers'], False),
        ('leaked-managed-bytes', ['--php', php_wrapper('leakbytes')], 'connect-10-mux', 1, ['leaked_managed_bytes'], False),
        ('thread-growth', ['--php', php_wrapper('threads')], 'connect-10-mux', 1, ['thread_growth'], False),
        ('fd-growth', ['--php', php_wrapper('fds')], 'connect-10-mux', 1, ['fd_growth'], False),
        ('client-connect-failure', ['--client', client_wrapper('fail1')], 'connect-10-mux', 1, ['connect_failures'], False),
        ('client-reliable-order-violation', ['--client', client_wrapper('order')], 'connect-10-mux', 1, ['order violations'], False),
        ('client-flood-order-violation', ['--client', client_wrapper('floodorder')], 'connect-10-mux', 1, ['order violations'], False),
        ('client-cleanup-failed', ['--client', client_wrapper('cleanup')], 'connect-10-mux', 1, ['cleanup_ok'], False),
        ('client-unreliable-loss', ['--client', client_wrapper('unrelloss')], 'connect-10-mux', 1, ['unreliable echo loss'], False),
        ('client-reliable-unanswered', ['--client', client_wrapper('relunanswered')], 'connect-10-mux', 1, ['reliable echo unanswered'], False),
        ('client-dropped-peers', ['--client', client_wrapper('dropped')], 'connect-10-mux', 1, ['peers dropped'], False),
        ('client-crash-without-json', ['--client', client_wrapper('crash')], 'connect-10-mux', 1, ['client_c'], False),
        ('client-nonzero-exit', ['--client', client_wrapper('exitcode')], 'connect-10-mux', 1, ['exit code'], False),
        ('tolerated-handshake-failure', ['--client', client_wrapper('fail1'), '--max-connect-fail-pct', '10'], 'connect-20-mux', 0, 'TOLERATED', True),
        ('handshake-failures-over-tolerance', ['--client', client_wrapper('fail5'), '--max-connect-fail-pct', '10'], 'connect-20-mux', 1, ['connect_failures'], True),
        # the limits were calibrated with the default 12 s hold, a shorter window makes p99 noisier
        ('server-receive-overflow', ['--php', php_wrapper('overflow')], 'connect-10-mux', 1, ['receive_overflow'], True),
        ('server-exception-counters', ['--php', php_wrapper('exc')], 'connect-10-mux', 1, ['echo_exceptions'], True),
        ('server-without-samples', ['--php', php_wrapper('nosamples')], 'connect-10-mux', 1, ['no_samples'], True),
        ('soak-fd-growth', ['--php', php_wrapper('soakgrowth'), '--soak-seconds', '20'], 'soak-10-mux', 1, ['soak_fd_growth'], True),
        ('client-large-message-failed', ['--client', client_wrapper('largefail')], 'connect-10-mux', 1, ['large messages failed'], True),
        ('client-errors', ['--client', client_wrapper('errors')], 'connect-10-mux', 1, ['errors'], True),
        ('abrupt-victims-never-connect', ['--client', client_wrapper('novictim'), '--abrupt-seconds', '20', '--abrupt-connect-wait', '15'], 'abrupt-10-mux', 1, ['abrupt'], True),
        ('abrupt-victims-gone-before-the-kill', ['--client', client_wrapper('shortvictim'), '--abrupt-seconds', '20'], 'abrupt-10-mux', 1, ['abrupt_victims_missing'], True),
        ('slow-fast-peers-without-progress', ['--client', client_wrapper('norttn'), '--slow-seconds', '8', '--freeze-seconds', '4'], 'slow-10-mux', 1, ['fast_peers_no_progress'], True),
        ('drain-limit-exceeded', ['--drain-limit', '0'], 'connect-10-mux', 1, ['not_drained'], True),
        ('repeat-zero-runs-nothing', ['--repeat', '0'], 'connect-10-mux', 2, None, True),
        ('latency-limit-follows-the-scenario-not-its-spelling', ['--php', php_wrapper('idle40'), '--hold', '12'], 'connect-100', 1, ['latency_p95', 'latency_p99'], True),
        ('latency-positive-control', ['--hold', '12'], 'connect-100-mux', 0, None, True),
        ('latency-regression', ['--php', php_wrapper('idle40'), '--hold', '12'], 'connect-100-mux', 1, ['latency_p95', 'latency_p99'], True),
    ]
    only = set(filter(None, args.only.split(',')))
    results = []
    for name, extra, scenario, want, text, needs_new in cases:
        if only and name not in only:
            continue
        if args.legacy and needs_new:
            results.append((name, want, 'skipped', 'SKIP'))
            continue
        run_out = os.path.join(out, name)
        cmd = [sys.executable, args.stress, '--module', args.module, '--client', args.client, '--out', run_out] + base
        # later options win in argparse, so case specific values override the defaults
        cmd += extra + [scenario]
        t0 = time.time()
        try:
            proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60 if args.legacy else 240)
            got = proc.returncode
            log = proc.stdout.decode('utf-8', 'replace')
        except subprocess.TimeoutExpired:
            got, log = 'HUNG', ''
        open(os.path.join(out, name + '.log'), 'w').write(log)
        verdict = {}
        try:
            verdict = json.load(open(os.path.join(run_out, 'verdict.json')))
        except Exception:
            pass
        ok = got == want
        note = ''
        if ok and text and not args.legacy:
            flat = json.dumps(verdict.get('scenarios', []))
            texts = [text] if isinstance(text, str) else text
            if not any(t in flat for t in texts):
                ok, note = False, f'exit {got} but verdict.json names none of {texts}'
        results.append((name, want, got, 'OK' if ok else ('NOT DETECTED' if got == 0 and want != 0 else ('HARNESS HUNG' if got == 'HUNG' else 'WRONG')) + (' ' + note if note else ''), round(time.time() - t0)))

    # run-all.sh must stop at the first failing group and with --keep-going run the others, both ending non-zero
    if not only or 'run-all' in only:
        for name, flag, groups in (('run-all-fail-fast', [], 1), ('run-all-keep-going', ['--keep-going'], 2)):
            if args.legacy and flag:
                continue
            run_out = os.path.join(out, name)
            try:
                proc = subprocess.run(['bash', os.path.join(os.path.dirname(os.path.abspath(args.stress)), 'run-all.sh')] + flag + ['/nonexistent/rtc.so', run_out, '1'],
                                      env=dict(os.environ, RTC_STRESS_CLIENT=os.path.abspath(args.client)), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=400 if args.legacy else 120)
            except subprocess.TimeoutExpired:
                results.append((name, 1, 'HUNG', 'HARNESS HUNG', 0))
                continue
            seen = len(open(os.path.join(run_out, 'groups.txt')).read().splitlines()) if os.path.exists(os.path.join(run_out, 'groups.txt')) else 0
            ok = proc.returncode == 1 and (seen == groups or args.legacy)
            results.append((name, 1, proc.returncode, 'OK' if ok else ('NOT DETECTED' if proc.returncode == 0 else f'WRONG groups run {seen} want {groups}'), 0))

    # the latency limits must follow the recorded baseline runs, a table edited without data is rejected
    if not only or 'calibrate' in only:
        scratch = os.path.join(out, 'calibrate')
        shutil.copytree(HERE, scratch, ignore=shutil.ignore_patterns('__pycache__', 'results'))
        good = subprocess.run([sys.executable, os.path.join(scratch, 'calibrate.py'), 'check'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT).returncode
        results.append(('limits-match-the-recorded-baseline', 0, good, 'OK' if good == 0 else 'WRONG', 0))
        text = open(os.path.join(scratch, 'stress.py')).read()
        open(os.path.join(scratch, 'stress.py'), 'w').write(text.replace("'connect-100-mux': (5, 7)", "'connect-100-mux': (500, 700)"))
        bad = subprocess.run([sys.executable, os.path.join(scratch, 'calibrate.py'), 'check'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT).returncode
        results.append(('limits-edited-without-data-are-rejected', 1, bad, 'OK' if bad == 1 else ('NOT DETECTED' if bad == 0 else 'WRONG'), 0))

    width = max(len(r[0]) for r in results)
    print(f'{"case".ljust(width)}  want  got   result')
    bad = 0
    for r in results:
        print(f'{r[0].ljust(width)}  {str(r[1]).ljust(4)}  {str(r[2]).ljust(4)}  {r[3]}')
        if r[3] not in ('OK', 'SKIP'):
            bad += 1
    print(f'\n{len(results) - bad}/{len(results)} cases behaved as expected' + (' (legacy mode)' if args.legacy else ''))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
