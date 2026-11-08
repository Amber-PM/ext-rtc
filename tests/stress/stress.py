#!/usr/bin/env python3
"""ext-rtc stress orchestrator

usage: stress.py --module rtc.so --client rtc-stress-client --out DIR [--php php] SCENARIO [SCENARIO...]
scenarios: connect-100 connect-300 connect-500 (optionally suffixed -mux/-ports), reconnect-100 reconnect-300,
           abrupt-100, slow-100, both-100, soak (use --soak-seconds)
every run writes DIR/<scenario>.run<k>.json and DIR/summary.md
"""
import argparse, json, os, signal, socket, statistics, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
# every child the run starts, so a crashed or timed out scenario never leaves clients or servers behind
PROCS = []


class HarnessError(Exception):
    pass


def reap():
    """kill every child group, including stopped or orphaned grandchildren of wrapper commands"""
    for p in PROCS:
        for sig in (signal.SIGCONT, signal.SIGKILL):
            try:
                os.killpg(p.pid, sig)
            except (ProcessLookupError, PermissionError):
                pass
        try:
            p.wait(timeout=5)
        except Exception:
            pass
    del PROCS[:]


def free_port(kind):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM if kind == 'tcp' else socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


def load():
    return open('/proc/loadavg').read().split()[:3]


_FFI = {}


def ffi_args(php):
    """options that load FFI for the allocator counters of the server, empty when this PHP has no FFI"""
    if php not in _FFI:
        try:
            out = subprocess.run([php, '-n', '-d', 'extension=ffi', '-d', 'ffi.enable=1', '-r', 'echo extension_loaded("ffi") ? "1" : "0";'],
                                 stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=20).stdout.decode().strip()
        except Exception:
            out = '0'
        _FFI[php] = ['-d', 'extension=ffi', '-d', 'ffi.enable=1'] if out == '1' else []
    return _FFI[php]


class Server:
    def __init__(self, args, mode, tag, extra=None):
        self.port = free_port('tcp')
        self.ice = free_port('udp')
        self.samples = os.path.join(args.out, f'{tag}.samples.jsonl')
        cmd = [args.php, '-n'] + ffi_args(args.php) + ['-d', 'extension=' + args.module, os.path.join(HERE, 'server.php'),
               f'--port={self.port}', f'--mode={mode}', f'--ice-port={self.ice}', f'--samples={self.samples}']
        cmd += extra or []
        self.out = open(os.path.join(args.out, f'{tag}.server.out'), 'w')
        self.stats_timeout = args.stats_timeout
        self.quit_timeout = args.quit_timeout
        self.proc = subprocess.Popen(cmd, stdout=self.out, stderr=subprocess.STDOUT, start_new_session=True)
        PROCS.append(self.proc)
        self.t0 = time.time()
        ready = False
        for _ in range(100):
            time.sleep(0.1)
            if self.proc.poll() is not None:
                break
            if os.path.getsize(self.out.name) > 0 and 'READY' in open(self.out.name).read():
                ready = True
                break
        if not ready:
            self.out.flush()
            tail = open(self.out.name).read()[-300:].strip()
            raise HarnessError('server_not_ready rc=%s %s' % (self.proc.poll(), tail))
        self.ctl = socket.create_connection(('127.0.0.1', self.port))
        self.ctl.sendall(b'HELLO ctl\n')
        self.buf = b''

    def stats(self):
        self.ctl.sendall(b'STATS\n')
        self.ctl.settimeout(self.stats_timeout)
        while b'\n' not in self.buf:
            chunk = self.ctl.recv(1 << 20)
            if not chunk:
                raise HarnessError('server_closed_control_connection rc=%s' % self.proc.poll())
            self.buf += chunk
        line, self.buf = self.buf.split(b'\n', 1)
        return json.loads(line)

    killed = False

    def alive(self):
        return self.proc.poll() is None

    def finish(self):
        final = None
        if self.alive():
            try:
                self.ctl.sendall(b'QUIT\n')
                self.proc.wait(timeout=self.quit_timeout)
            except Exception:
                self.proc.kill()
                self.proc.wait()
                self.killed = True
        self.out.close()
        for line in open(self.out.name):
            if line.startswith('FINAL '):
                final = json.loads(line[6:])
        return final, (-9 if self.killed else self.proc.returncode)


def spawn_client(args, srv, tag, name, **kw):
    out = os.path.join(args.out, f'{tag}.{name}.json')
    cmd = [args.client, '--server', f'127.0.0.1:{srv.port}', '--out', out, '--name', name]
    for k, v in kw.items():
        cmd += ['--' + k.replace('_', '-'), str(v)]
    err = open(os.path.join(args.out, f'{tag}.{name}.err'), 'w')
    p = subprocess.Popen(cmd, stderr=err, stdout=subprocess.DEVNULL, start_new_session=True)
    PROCS.append(p)
    p.json_path = out
    p.started = time.time()
    return p


def wait_client(p, timeout):
    try:
        p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
        return {'error': 'client_timeout'}
    if p.returncode != 0 and not os.path.exists(p.json_path):
        return {'error': f'client_exit_{p.returncode}'}
    try:
        d = json.load(open(p.json_path))
    except Exception:
        return {'error': 'client_no_json'}
    if p.returncode != 0:
        d['exit_code'] = p.returncode
    return d


def read_samples(path):
    out = []
    if not os.path.exists(path):
        return out
    for line in open(path):
        try:
            out.append(json.loads(line))
        except Exception:
            pass
    return out


def slope(xs, ys):
    if len(xs) < 3:
        return 0.0
    mx, my = statistics.mean(xs), statistics.mean(ys)
    den = sum((x - mx) ** 2 for x in xs)
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den if den else 0.0


def server_summary(samples, final):
    """cpu, rss, resource trend and queue evidence from the server side"""
    if not samples:
        return {}
    busy = [s for s in samples if s['peers_local'] > 0]
    base = samples[0]
    res = {'rss_start_mb': round(base['rss_kb'] / 1024, 1), 'rss_peak_mb': round(max(s['hwm_kb'] for s in samples) / 1024, 1),
           'rss_end_mb': round(samples[-1]['rss_kb'] / 1024, 1), 'threads_peak': max(s['threads'] for s in samples),
           'threads_end': samples[-1]['threads'], 'fds_peak': max(s['fds'] for s in samples), 'fds_end': samples[-1]['fds'],
           'peers_peak': max(s['peers_local'] for s in samples),
           'managed_peak_kb': round(max(s['runtime']['managedBufferBytes'] for s in samples) / 1024, 1)}
    if len(busy) >= 3:
        a, b = busy[len(busy) // 5], busy[-1 - len(busy) // 10]
        dt = b['t'] - a['t']
        if dt > 0:
            cpu = (b['cpu_user_s'] + b['cpu_sys_s']) - (a['cpu_user_s'] + a['cpu_sys_s'])
            res['cpu_pct_steady'] = round(100 * cpu / dt, 1)
            res['cpu_user_pct_steady'] = round(100 * (b['cpu_user_s'] - a['cpu_user_s']) / dt, 1)
            res['cpu_sys_pct_steady'] = round(100 * (b['cpu_sys_s'] - a['cpu_sys_s']) / dt, 1)
            res['echo_per_s'] = round((b['counters']['echo'] - a['counters']['echo']) / dt)
    if final:
        c = final['counters']
        res['cpu_pct_whole'] = round(100 * (final['cpu_user_s'] + final['cpu_sys_s']) / final['t'], 1)
        res['final_active_peers'] = final['runtime']['activePeers']
        res['final_active_channels'] = final['runtime']['activeChannels']
        res['final_managed_bytes'] = final['runtime']['managedBufferBytes']
        res['receive_overflow'] = final['runtime']['receiveOverflowCount']
        res['send_rejected_native'] = final['runtime']['sendRejectedCount']
        for k in ('accepted', 'rejected_offers', 'connected', 'closed', 'closed_failed', 'echo', 'echo_dropped_false', 'echo_exceptions', 'echo_closed_races',
                  'recv_rel_bad', 'recv_unrel_lost', 'stream_rejected_rel', 'stream_rejected_unrel', 'flood_sent', 'flood_rejected',
                  'flood_budget', 'flood_done', 'loop_max_ms', 'loop_over_10ms', 'loop_over_100ms', 'create_max_ms', 'close_max_ms',
                  'receive_exceptions', 'poll_exceptions'):
            res[k] = c[k]
        res['exceptions'] = c['send_exceptions']
        res['loop_busy_pct'] = round(100 * c['busy_ns'] / 1e9 / final['t'], 1)
    return res


def pct(v, p):
    if not v:
        return None
    v = sorted(v)
    return v[min(len(v) - 1, max(0, int(-(-p * len(v) // 1)) - 1))]


def window_p99(client, t_from, t_to):
    """worst per-second reliable p99 in [t_from, t_to) client seconds, plus median of per-second p50"""
    rows = [r for r in client.get('timeline', []) if t_from <= r['t'] < t_to and r['rel_n'] > 0]
    if not rows:
        return None
    return {'p99_us_max': max(r['rel_p99'] for r in rows), 'p50_us_med': statistics.median(r['rel_p50'] for r in rows), 'seconds': len(rows)}


# ---------------------------------------------------------------- scenarios

def sc_connect(args, tag, n, mode):
    srv = Server(args, mode, tag)
    t0 = time.time()
    c = spawn_client(args, srv, tag, 'c', count=n, parallel=args.parallel, duration=args.hold, echo_rate=10, size=64, connect_timeout=45)
    cd = wait_client(c, 240 + n)
    idle = drain(srv, args.drain_limit)
    final, rc = srv.finish()
    return {'clients': {'c': cd}, 'server_rc': rc, 'drain_s': idle}, srv, final


def drain(srv, limit):
    t0 = time.time()
    while time.time() - t0 < limit:
        try:
            s = srv.stats()
        except Exception:
            return None
        if s['peers_local'] == 0 and s['runtime']['activePeers'] == 0:
            return round(time.time() - t0, 1)
        time.sleep(0.5)
    return None


def sc_reconnect(args, tag, n, mode):
    srv = Server(args, mode, tag)
    c = spawn_client(args, srv, tag, 'c', count=n, parallel=args.parallel, rounds=args.rounds, hold=1, echo_rate=10, gap_ms=200, connect_timeout=45)
    cd = wait_client(c, 120 + n * args.rounds)
    idle = drain(srv, args.drain_limit)
    final, rc = srv.finish()
    return {'clients': {'c': cd}, 'server_rc': rc, 'drain_s': idle}, srv, final


def sc_both(args, tag, n, mode):
    srv = Server(args, mode, tag)
    c = spawn_client(args, srv, tag, 'c', count=n, parallel=args.parallel, duration=args.hold + 5, echo_rate=5, stream_rate=100, size=512, connect_timeout=45)
    cd = wait_client(c, 200 + n)
    idle = drain(srv, args.drain_limit)
    final, rc = srv.finish()
    return {'clients': {'c': cd}, 'server_rc': rc, 'drain_s': idle}, srv, final


def sc_abrupt(args, tag, n, mode):
    """a stable half keeps echoing while the other half is killed with SIGKILL, then reconnects"""
    srv = Server(args, mode, tag)
    half = n // 2
    a = spawn_client(args, srv, tag, 'stable', count=half, parallel=args.parallel, duration=args.abrupt_seconds, echo_rate=10, connect_timeout=45)
    b = spawn_client(args, srv, tag, 'victim', count=half, parallel=args.parallel, duration=3600, echo_rate=10, connect_timeout=45)
    # wait until both groups are connected, killing nothing proves nothing
    deadline = time.time() + args.abrupt_connect_wait
    while True:
        s = srv.stats()
        if s['counters']['connected'] >= n:
            break
        if time.time() > deadline:
            b.kill()
            a.kill()
            raise RuntimeError(f"abrupt: only {s['counters']['connected']} of {n} peers connected before the kill")
        time.sleep(0.5)
    time.sleep(4)
    before_kill = srv.stats()['peers_local']
    kill_at = time.time()
    b.send_signal(signal.SIGKILL)
    b.wait()
    detect = None
    track = []
    t_kill_srv = srv.stats()
    while time.time() - kill_at < args.abrupt_seconds - 8:
        s = srv.stats()
        track.append({'dt': round(time.time() - kill_at, 1), 'peers': s['peers_local'], 'active': s['runtime']['activePeers']})
        if detect is None and s['peers_local'] <= half:
            detect = round(time.time() - kill_at, 1)
        if time.time() - kill_at > 6 and detect is None and len(track) > 10 and False:
            break
        time.sleep(1)
        if time.time() - kill_at > 12 and detect is not None:
            break
    # reconnect a fresh victim group while the stable group is still running
    b2 = spawn_client(args, srv, tag, 'victim2', count=half, parallel=args.parallel, duration=5, echo_rate=10, connect_timeout=45)
    b2d = wait_client(b2, 120)
    ad = wait_client(a, args.abrupt_seconds + 120)
    idle = drain(srv, args.drain_limit + 30)
    final, rc = srv.finish()
    ad['kill_at_client_s'] = round(kill_at - a.started, 1)
    return {'clients': {'stable': ad, 'victim2': b2d}, 'server_rc': rc, 'drain_s': idle, 'kill_detect_s': detect, 'kill_track': track[:60], 'peers_before_kill': before_kill, 'peers_expected': n,
            'stable_before_kill': window_p99(ad, 1, max(2, int(kill_at - a.started))), 'stable_after_kill': window_p99(ad, int(kill_at - a.started), int(kill_at - a.started) + 20)}, srv, final


def sc_slow(args, tag, n, mode, mode_slow='stop'):
    """fast peers echo while a group of slow peers receives a server flood and then freezes (SIGSTOP) or reads slowly"""
    srv = Server(args, mode, tag)
    slow_n = max(4, n // 10)
    a = spawn_client(args, srv, tag, 'fast', count=n - slow_n, parallel=args.parallel, duration=args.slow_seconds, echo_rate=10, connect_timeout=45)
    kw = dict(count=slow_n, parallel=args.parallel, duration=args.slow_seconds, echo_rate=0, flood_count=1000000000, flood_size=1000, connect_timeout=45)
    if mode_slow == 'delay':
        kw.update(slow_peers=slow_n, read_delay_ms=200)
    b = spawn_client(args, srv, tag, 'slow', **kw)
    deadline = time.time() + 120
    while time.time() < deadline:
        s = srv.stats()
        if s['counters']['connected'] >= n:
            break
        time.sleep(0.5)
    time.sleep(6)
    stop_at = time.time()
    if mode_slow == 'stop':
        b.send_signal(signal.SIGSTOP)
    time.sleep(args.freeze_seconds)
    mid = srv.stats()
    if mode_slow == 'stop':
        b.send_signal(signal.SIGCONT)
    resume_at = time.time()
    ad = wait_client(a, args.slow_seconds + 120)
    bd = wait_client(b, args.slow_seconds + 120)
    idle = drain(srv, args.drain_limit)
    final, rc = srv.finish()
    s0 = int(stop_at - a.started)
    s1 = int(resume_at - a.started)
    res = {'clients': {'fast': ad, 'slow': bd}, 'server_rc': rc, 'drain_s': idle, 'mode_slow': mode_slow,
           'fast_before': window_p99(ad, 2, s0), 'fast_during_freeze': window_p99(ad, s0, s1), 'fast_after': window_p99(ad, s1 + 1, s1 + 15),
           'server_at_freeze_end': {k: mid['counters'][k] for k in ('flood_sent', 'flood_rejected', 'flood_budget', 'loop_max_ms')} | {'managed_kb': round(mid['runtime']['managedBufferBytes'] / 1024), 'rss_mb': round(mid['rss_kb'] / 1024, 1)}}
    return res, srv, final


def sc_soak(args, tag, n, mode):
    srv = Server(args, mode, tag)
    c = spawn_client(args, srv, tag, 'c', count=n, parallel=args.parallel, duration=args.soak_seconds, echo_rate=5, churn_frac=0.1, churn_interval=10, connect_timeout=45)
    cd = wait_client(c, args.soak_seconds + 300)
    idle = drain(srv, args.drain_limit)
    final, rc = srv.finish()
    return {'clients': {'c': cd}, 'server_rc': rc, 'drain_s': idle}, srv, final


def sc_interop(args, tag, n, mode):
    """independent aiortc peers (n) next to native libdatachannel peers (n) on the same server"""
    srv = Server(args, mode, tag)
    env = dict(os.environ, PYTHONPATH=args.pylib)
    out = os.path.join(args.out, f'{tag}.aiortc.json')
    err = open(os.path.join(args.out, f'{tag}.aiortc.err'), 'w')
    p = subprocess.Popen([sys.executable, os.path.join(HERE, 'interop', 'aiortc_client.py'), '--server', f'127.0.0.1:{srv.port}',
                          '--count', str(n), '--duration', '10', '--out', out], env=env, stderr=err, start_new_session=True)
    PROCS.append(p)
    p.json_path = out
    c = spawn_client(args, srv, tag, 'c', count=n, parallel=n, duration=10, echo_rate=10, connect_timeout=45)
    cd = wait_client(c, 120)
    ad = wait_client(p, 180)
    idle = drain(srv, args.drain_limit)
    final, rc = srv.finish()
    return {'clients': {'c': cd, 'aiortc': ad}, 'server_rc': rc, 'drain_s': idle}, srv, final


SCENARIOS = {'interop': sc_interop, 'connect': sc_connect, 'reconnect': sc_reconnect, 'both': sc_both, 'abrupt': sc_abrupt, 'slow': sc_slow, 'slowdelay': lambda a, t, n, m: sc_slow(a, t, n, m, 'delay'), 'soak': sc_soak}


# default latency limits in ms for the reliable echo round trip: (p95, p99) per canonical scenario kind-n-mode
# derived by calibrate.py from latency-baseline.json, the recorded runs of the original module on an idle machine
# a limit is LATENCY_MARGIN times the worst recorded value, `calibrate.py check` fails when this table and the recorded runs disagree
# the margin is 3 because healthy runs of the final module under a machine load of 1.6 to 3 reached 2.7 times the idle worst value,
# the regressions that this table must catch are far larger: the four sender dispatcher measured p95 183 to 257 ms against a limit of 57 ms
LATENCY_MARGIN = 3.0
LATENCY_LIMITS_MS = {
    'abrupt-100-mux': (6, 8),
    'abrupt-100-ports': (6, 7),
    'both-100-mux': (24, 59),
    'connect-100-mux': (5, 7),
    'connect-100-ports': (4, 7),
    'connect-300-mux': (12, 69),
    'connect-300-ports': (9, 37),
    'connect-500-mux': (36, 89),
    'connect-500-ports': (31, 73),
    'reconnect-100-mux': (13, 34),
    'reconnect-100-ports': (14, 27),
    'reconnect-300-mux': (93, 165),
    'slow-100-mux': (28, 41),
    'slowdelay-100-mux': (4, 6),
    'soak-100-mux': (5, 14),
}
# worst per second p99 of the fast peers while the others are frozen, same derivation and margin
FAST_FREEZE_P99_LIMIT_MS = {'slow-100-mux': 73, 'slowdelay-100-mux': 7}


def evaluate(kind, args, res, final, samples, scenario=None):
    """explicit pass/fail criteria, returns (failures, tolerated, warnings) as lists of strings"""
    fail, tolerated, warn = [], [], []
    slow = kind in ('slow', 'slowdelay')
    clients = res.get('clients', {})
    rc = res.get('server_rc')
    if final is None:
        fail.append(f'server_no_final: crash or hang, rc={rc}')
    elif rc != 0:
        fail.append(f'server_exit_code: rc={rc} after QUIT')
    attempted = failed = ok = 0
    for name, c in clients.items():
        if 'error' in c:
            fail.append(f'client_{name}: {c["error"]}')
            continue
        if c.get('exit_code'):
            fail.append(f'client_{name}: exit code {c["exit_code"]}')
        if c.get('cleanup_ok') is False:
            fail.append(f'client_{name}: cleanup_ok=false (native teardown did not finish)')
        con = c.get('connect', {})
        attempted += con.get('attempted', 0)
        failed += con.get('fail', 0)
        ok += con.get('ok', 0)
        if c.get('dropped_peers', 0) and not (slow and name == 'slow'):
            fail.append(f'client_{name}: {c["dropped_peers"]} peers dropped by the server')
        if c.get('errors'):
            fail.append(f'client_{name}: errors {c["errors"][:3]}')
        if c.get('large_message_fail'):
            fail.append(f'client_{name}: {c["large_message_fail"]} large messages failed')
        st = c.get('stream', {})
        if st.get('rel_order_violations'):
            fail.append(f'client_{name}: {st["rel_order_violations"]} reliable order violations')
        if c.get('flood', {}).get('order_violations'):
            fail.append(f'client_{name}: {c["flood"]["order_violations"]} reliable flood order violations')
        e = c.get('echo', {})
        if e.get('unrel_loss_pct', 0) > args.max_unrel_loss_pct:
            fail.append(f'client_{name}: unreliable echo loss {e["unrel_loss_pct"]}% > {args.max_unrel_loss_pct}%')
        total = st.get('unrel_lost', 0) + st.get('unrel_recv', 0)
        if total and 100.0 * st.get('unrel_lost', 0) / total > args.max_unrel_loss_pct:
            fail.append(f'client_{name}: unreliable stream loss {st["unrel_lost"]}/{total} > {args.max_unrel_loss_pct}%')
        if e.get('rel_sent', 0):
            gone = e['rel_sent'] - e.get('rel_recv', 0)
            if gone < 0 or 100.0 * gone / e['rel_sent'] > args.max_rel_unanswered_pct:
                fail.append(f'client_{name}: reliable echo unanswered {gone}/{e["rel_sent"]} > {args.max_rel_unanswered_pct}%')
            elif gone:
                warn.append(f'client_{name}: reliable echo unanswered {gone}/{e["rel_sent"]} (in flight at close)')
    if not any('error' in c for c in clients.values()) and (attempted == 0 or ok == 0):
        fail.append('no_connections: nothing connected')
    allowed = int(args.max_connect_fail_pct * attempted / 100.0)
    if failed > allowed:
        fail.append(f'connect_failures: {failed} of {attempted} handshakes failed (allowed {allowed} at {args.max_connect_fail_pct}%)')
    elif failed:
        tolerated.append(f'connect_failures: {failed} of {attempted} handshakes failed, within --max-connect-fail-pct {args.max_connect_fail_pct}')
    if 'drain_s' in res and res['drain_s'] is None:
        fail.append('not_drained: server still had active peers after the drain limit')
    if final:
        rt, c = final['runtime'], final['counters']
        for key, label in (('activePeers', 'leaked_peers'), ('activeChannels', 'leaked_channels'), ('managedBufferBytes', 'leaked_managed_bytes')):
            if rt.get(key):
                fail.append(f'{label}: {key}={rt[key]} after shutdown of all peers')
        if rt.get('receiveOverflowCount'):
            fail.append(f'receive_overflow: {rt["receiveOverflowCount"]} reliable channels were closed on queue overflow')
        for key in ('echo_exceptions', 'receive_exceptions', 'poll_exceptions', 'bad_channels', 'recv_rel_bad'):
            if c.get(key):
                fail.append(f'server_{key}: {c[key]}')
        if c.get('rejected_offers'):
            warn.append(f'rejected_offers: {c["rejected_offers"]}')
        if c.get('echo_closed_races'):
            warn.append(f'echo_closed_races: {c["echo_closed_races"]} echoes met a peer that the client closed in the same moment')
        if c.get('echo_dropped_false'):
            warn.append(f'echo_dropped_false: {c["echo_dropped_false"]} echo replies refused by the send queue')
    if samples:
        # a sample taken while /proc/self cannot be read (fd exhaustion) reports 0 threads or -1 fds, it says nothing about growth
        good_t = [s['threads'] for s in samples if s['threads'] > 0]
        good_f = [s['fds'] for s in samples if s['fds'] >= 0]
        if good_t and samples[-1]['threads'] > min(good_t) + args.thread_slack:
            fail.append(f'thread_growth: {min(good_t)} -> {samples[-1]["threads"]} threads at the end')
        if good_f and samples[-1]['fds'] > min(good_f) + args.thread_slack:
            fail.append(f'fd_growth: {min(good_f)} -> {samples[-1]["fds"]} fds at the end')
        if len(good_t) != len(samples) or len(good_f) != len(samples):
            warn.append(f'unreadable_samples: {len(samples) - min(len(good_t), len(good_f))} of {len(samples)} resource samples failed')
        if kind == 'soak':
            busy = samples[int(len(samples) * 0.3):]
            ts = [s['t'] for s in busy]
            for key, label in (('fds', 'fd'), ('threads', 'thread')):
                sl = slope(ts, [s[key] for s in busy]) * 60
                if sl > args.max_slope_per_min:
                    fail.append(f'soak_{label}_growth: {sl:.2f}/min > {args.max_slope_per_min}')
            kb = slope(ts, [s['rss_kb'] for s in busy]) * 60
            if args.max_rss_slope_kb_min is not None and kb > args.max_rss_slope_kb_min:
                fail.append(f'soak_rss_growth: {kb:.0f} KB/min > {args.max_rss_slope_kb_min}')
            elif kb > 300:
                warn.append(f'soak_rss_growth: {kb:.0f} KB/min (leak or fragmentation, needs a longer soak)')
    elif final is not None:
        fail.append('no_samples: server wrote no resource samples')
    if kind == 'abrupt' and res.get('peers_before_kill', 0) < res.get('peers_expected', 1):
        fail.append(f'abrupt_victims_missing: the server held {res.get("peers_before_kill")} of {res.get("peers_expected")} peers when the victims were killed')
    if kind == 'abrupt' and res.get('kill_detect_s') is None:
        fail.append('killed_peers_not_released: the server never noticed the SIGKILLed clients')
    if slow:
        fast = clients.get('fast', {})
        if not fast.get('rtt_reliable', {}).get('n'):
            fail.append('fast_peers_no_progress: no reliable echo completed for the fast peers')
        elif res.get('fast_during_freeze') is None:
            fail.append('fast_peers_no_progress_during_freeze: no echo answered while the slow peers were frozen')
        elif args.max_fast_p99_ms is not None and res['fast_during_freeze']['p99_us_max'] / 1000.0 > args.max_fast_p99_ms:
            fail.append(f'fast_peers_latency: p99 {res["fast_during_freeze"]["p99_us_max"] / 1000.0:.1f} ms > {args.max_fast_p99_ms} ms while slow peers frozen')
    if not args.no_latency_limits and scenario in LATENCY_LIMITS_MS:
        main = clients.get('c') or clients.get('fast') or clients.get('stable') or next(iter(clients.values()), {})
        rtt = main.get('rtt_reliable') or {}
        if rtt.get('n'):
            for label, limit in zip(('p95', 'p99'), LATENCY_LIMITS_MS[scenario]):
                value = rtt.get(label + '_us', 0) / 1000.0
                if value > limit * args.latency_scale:
                    fail.append(f'latency_{label}: reliable echo {label} {value:.1f} ms > {limit * args.latency_scale:.1f} ms limit of {scenario}'
                                f' (machine load {load()[0]}, the limits assume an idle machine, --latency-scale adapts them)')
        else:
            fail.append('latency_no_samples: no reliable echo round trip was measured')
    if slow and not args.no_latency_limits and args.max_fast_p99_ms is None and scenario in FAST_FREEZE_P99_LIMIT_MS and res.get('fast_during_freeze'):
        value = res['fast_during_freeze']['p99_us_max'] / 1000.0
        limit = FAST_FREEZE_P99_LIMIT_MS[scenario] * args.latency_scale
        if value > limit:
            fail.append(f'fast_peers_latency: p99 {value:.1f} ms > {limit:.1f} ms limit of {scenario} while the others are frozen')
    return fail, tolerated, warn


def summarize(name, res, srv, final, samples):
    row = {'scenario': name}
    srvsum = server_summary(samples, final)
    row['server'] = srvsum
    cl = res['clients']
    main = cl.get('c') or cl.get('fast') or cl.get('stable') or next(iter(cl.values()))
    if 'error' in main:
        row['error'] = main['error']
        return row
    row['connect_ok'] = sum(c.get('connect', {}).get('ok', 0) for c in cl.values() if 'connect' in c)
    row['connect_fail'] = sum(c.get('connect', {}).get('fail', 0) for c in cl.values() if 'connect' in c)
    row['connect_ms'] = main['connect']['ms']
    row['rtt_rel'] = main['rtt_reliable']
    row['rtt_unrel'] = main['rtt_unreliable']
    row['unrel_loss_pct'] = main['echo']['unrel_loss_pct']
    row['stream'] = main['stream']
    row['dropped_peers'] = sum(c.get('dropped_peers', 0) for c in cl.values())
    row['flood_order_violations'] = sum(c.get('flood', {}).get('order_violations', 0) for c in cl.values())
    row['flood_recv'] = sum(c.get('flood', {}).get('recv', 0) for c in cl.values())
    row['bytes_in_mb'] = round(sum(c.get('bytes_in', 0) for c in cl.values()) / 1e6, 1)
    row['cleanup_ok'] = all(c.get('cleanup_ok', True) for c in cl.values())
    if 'rounds' in main and len(main['rounds']) > 1:
        row['rounds'] = [{'ok': r['ok'], 'fail': r['fail'], 'wall_ms': r['wall_ms'], 'p99_ms': r['connect_ms'].get('p99')} for r in main['rounds']]
    if 'aiortc' in cl:
        a = cl['aiortc']
        row['aiortc'] = {k: a.get(k) for k in ('connect', 'rtt_reliable', 'rtt_unreliable', 'echo', 'large_message_ok', 'large_message_fail', 'dropped_peers', 'errors')}
        row['aiortc']['rel_order_violations'] = a.get('stream', {}).get('rel_order_violations')
    for k in ('kill_detect_s', 'stable_before_kill', 'stable_after_kill', 'fast_before', 'fast_during_freeze', 'fast_after', 'server_at_freeze_end', 'kill_track', 'drain_s', 'server_rc', 'mode_slow'):
        if k in res:
            row[k] = res[k]
    if name.startswith('soak') and samples:
        busy = samples[int(len(samples) * 0.3):]
        row['rss_slope_kb_per_min'] = round(slope([s['t'] for s in busy], [s['rss_kb'] for s in busy]) * 60, 1)
        row['threads_slope'] = round(slope([s['t'] for s in busy], [s['threads'] for s in busy]) * 60, 2)
        row['fds_slope'] = round(slope([s['t'] for s in busy], [s['fds'] for s in busy]) * 60, 2)
        if all('heap_used_kb' in s for s in busy):
            row['heap_used_slope_kb_per_min'] = round(slope([s['t'] for s in busy], [s['heap_used_kb'] for s in busy]) * 60, 1)
            row['heap_free_slope_kb_per_min'] = round(slope([s['t'] for s in busy], [s['heap_free_kb'] for s in busy]) * 60, 1)
        if final and final.get('trim'):
            row['trim'] = final['trim']
    return row


def md_table(rows):
    cols = ['scenario', 'ok/fail', 'conn p50/p99 ms', 'srv CPU% (steady/whole)', 'RSS MB start/peak/end', 'rel RTT p50/p95/p99 us', 'unrel RTT p99 us', 'echo/s', 'rejected', 'unrel loss%', 'end peers/managed B', 'loop max ms']
    out = ['| ' + ' | '.join(cols) + ' |', '|' + '---|' * len(cols)]
    for r in rows:
        if 'error' in r:
            out.append(f"| {r['scenario']} | ERROR {r['error']} |" + ' |' * (len(cols) - 2))
            continue
        s = r['server']
        rej = s.get('echo_dropped_false', 0) + s.get('stream_rejected_rel', 0) + s.get('stream_rejected_unrel', 0) + s.get('flood_rejected', 0) + s.get('flood_budget', 0) + s.get('rejected_offers', 0)
        out.append('| ' + ' | '.join(map(str, [
            r['scenario'], f"{r['connect_ok']}/{r['connect_fail']}", f"{r['connect_ms'].get('p50')}/{r['connect_ms'].get('p99')}",
            f"{s.get('cpu_pct_steady')}/{s.get('cpu_pct_whole')}", f"{s.get('rss_start_mb')}/{s.get('rss_peak_mb')}/{s.get('rss_end_mb')}",
            f"{r['rtt_rel'].get('p50_us')}/{r['rtt_rel'].get('p95_us')}/{r['rtt_rel'].get('p99_us')}", r['rtt_unrel'].get('p99_us'),
            s.get('echo_per_s'), rej, r['unrel_loss_pct'], f"{s.get('final_active_peers')}/{s.get('final_managed_bytes')}", s.get('loop_max_ms')])) + ' |')
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--module', required=True)
    ap.add_argument('--client', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--php', default='php')
    ap.add_argument('--pylib', default=os.environ.get('RTC_STRESS_PYLIB', os.path.join(ROOT, '.build', 'stress', 'pylib')),
                    help='directory with aiortc, only for interop scenarios (pip install --target)')
    ap.add_argument('--repeat', type=int, default=1)
    ap.add_argument('--hold', type=int, default=12)
    ap.add_argument('--parallel', type=int, default=50)
    ap.add_argument('--rounds', type=int, default=5)
    ap.add_argument('--soak-seconds', type=int, default=300)
    ap.add_argument('--abrupt-seconds', type=int, default=60)
    ap.add_argument('--abrupt-connect-wait', type=int, default=120, help='seconds the abrupt scenario waits for all peers to connect before it fails')
    ap.add_argument('--drain-limit', type=float, default=60, help='seconds the server gets to release every peer after the clients are done, a longer drain fails the run')
    ap.add_argument('--slow-seconds', type=int, default=45)
    ap.add_argument('--freeze-seconds', type=int, default=15)
    g = ap.add_argument_group('pass/fail criteria (every violation fails the run with exit code 1)')
    g.add_argument('--stats-timeout', type=float, default=10, help='seconds a STATS request may take before the server counts as hung')
    g.add_argument('--quit-timeout', type=float, default=40, help='seconds the server gets to exit after QUIT before it is killed')
    g.add_argument('--max-connect-fail-pct', type=float, default=0.0,
                   help='tolerated failed handshakes in percent of attempts, default 0 (strict). Raw libdatachannel has a known DTLS handshake flake of about 1 in 2500-6000, '
                        'tolerated failures are reported as TOLERATED in verdict.json and the console, never silently')
    g.add_argument('--max-unrel-loss-pct', type=float, default=1.0, help='unreliable echo or stream loss above this fails')
    g.add_argument('--max-rel-unanswered-pct', type=float, default=0.5,
                   help='reliable echo requests without answer (in flight when a peer closes); the numbered reliable streams must have 0 order violations')
    g.add_argument('--thread-slack', type=int, default=4, help='threads or fds at the end above the lowest sample by more than this fails')
    g.add_argument('--max-slope-per-min', type=float, default=0.5, help='soak fd/thread growth per minute above this fails')
    g.add_argument('--max-rss-slope-kb-min', type=float, default=None, help='soak RSS growth limit, a warning at 300 KB/min when unset')
    g.add_argument('--max-fast-p99-ms', type=float, default=None, help='slow scenarios: worst per-second reliable p99 of the fast peers while the others are frozen')
    g.add_argument('--latency-scale', type=float, default=1.0, help='multiplies the default latency limits, for slower machines than the calibration one')
    g.add_argument('--no-latency-limits', action='store_true', help='disables the default latency limits of LATENCY_LIMITS_MS')
    ap.add_argument('--fail-fast', action='store_true', help='stop after the first failing run, by default every run is executed')
    ap.add_argument('scenarios', nargs='+')
    args = ap.parse_args()
    if args.repeat < 1:
        ap.error('--repeat must be at least 1, a gate that runs nothing proves nothing')
    for sc in args.scenarios:
        parts = sc.split('-')
        if parts[0] not in SCENARIOS or (len(parts) > 1 and not parts[1].isdigit()) or (len(parts) > 2 and parts[2] not in ('mux', 'ports')):
            ap.error('unknown scenario %s (kind-N-mode, kinds: %s)' % (sc, ', '.join(sorted(SCENARIOS))))
    for path, what in ((args.module, 'module'), (args.client, 'client')):
        if not os.path.isfile(path):
            print('stress: %s not found: %s' % (what, path), file=sys.stderr)
            return 2
    os.makedirs(args.out, exist_ok=True)
    rows = []
    env = {'nproc': os.cpu_count(), 'module': args.module, 'start_load': load(), 'time': time.strftime('%F %T')}
    stop = False
    try:
        for sc in args.scenarios:
            parts = sc.split('-')
            kind = parts[0]
            n = int(parts[1]) if len(parts) > 1 else 100
            mode = parts[2] if len(parts) > 2 else 'mux'
            canonical = f'{kind}-{n}-{mode}'
            for k in range(args.repeat):
                tag = f'{sc}.run{k}'
                load_before = load()
                fails, tolerated, warns = [], [], []
                try:
                    res, srv, final = SCENARIOS[kind](args, tag, n, mode)
                    samples = read_samples(srv.samples)
                    fails, tolerated, warns = evaluate(kind, args, res, final, samples, canonical)
                    try:
                        row = summarize(sc, res, srv, final, samples)
                    except Exception as e:
                        row = {'scenario': sc, 'server': server_summary(samples, final) if samples else {}}
                        fails.append(f'summary_error: {type(e).__name__}: {e}')
                    if not final:
                        row['error'] = 'server_no_final (crash or hang) rc=%s' % res.get('server_rc')
                except Exception as e:
                    row = {'scenario': sc}
                    fails.append(f'harness_exception: {type(e).__name__}: {e}')
                finally:
                    reap()
                row['run'] = k
                row['loadavg_before'] = load_before
                row['loadavg_after'] = load()
                row['verdict'] = 'FAIL' if fails else ('PASS_WITH_TOLERATED' if tolerated else 'PASS')
                row['failures'], row['tolerated'], row['warnings'] = fails, tolerated, warns
                json.dump(row, open(os.path.join(args.out, f'{tag}.json'), 'w'), indent=1)
                rows.append(row)
                try:
                    print(md_table([row]).splitlines()[-1], flush=True)
                except Exception:
                    print(f'| {sc} | run {k} |', flush=True)
                print(f'  -> {row["verdict"]} {sc} run{k}' + ''.join(f'\n     FAIL {f}' for f in fails) + ''.join(f'\n     TOLERATED {t}' for t in tolerated)
                      + ''.join(f'\n     warn {w}' for w in warns), flush=True)
                if fails and args.fail_fast:
                    stop = True
                    break
            if stop:
                break
    finally:
        reap()
    bad = [r for r in rows if r['verdict'] == 'FAIL']
    tol = [r for r in rows if r['tolerated']]
    overall = 'FAIL' if bad else ('PASS_WITH_TOLERATED' if tol else 'PASS')
    verdict = {'overall': overall, 'runs': len(rows), 'failed': len(bad), 'with_tolerated': len(tol), 'env': env,
               'criteria': {k: v for k, v in vars(args).items() if k.startswith('max_') or k in ('thread_slack', 'stats_timeout', 'quit_timeout')},
               'scenarios': [{'scenario': r['scenario'], 'run': r['run'], 'verdict': r['verdict'], 'failures': r['failures'], 'tolerated': r['tolerated'], 'warnings': r['warnings']} for r in rows]}
    json.dump(verdict, open(os.path.join(args.out, 'verdict.json'), 'w'), indent=1)
    lines = ['# ext-rtc stress summary', '', json.dumps(env), '', f'**verdict: {overall}** ({len(rows)} runs, {len(bad)} failed, {len(tol)} with tolerated handshake failures)', '']
    for r in rows:
        if r['verdict'] != 'PASS' or r['warnings']:
            lines.append(f'- {r["scenario"]} run{r["run"]}: {r["verdict"]}')
            lines += [f'  - FAIL {f}' for f in r['failures']] + [f'  - TOLERATED {t}' for t in r['tolerated']] + [f'  - warn {w}' for w in r['warnings']]
    try:
        lines += ['', md_table(rows)]
    except Exception as e:
        lines += ['', f'(table unavailable: {e})']
    open(os.path.join(args.out, 'summary.md'), 'w').write('\n'.join(lines) + '\n')
    print('\n'.join(lines))
    print(f'VERDICT: {overall}', file=sys.stderr)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
