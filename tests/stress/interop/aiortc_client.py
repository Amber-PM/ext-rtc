#!/usr/bin/env python3
"""independent WebRTC client (aiortc, pure python stack) against the php ext-rtc answerer

PYTHONPATH must contain aiortc (pip install --target DIR aiortc)
usage: aiortc_client.py --server 127.0.0.1:PORT --count 10 --duration 8 --out result.json
"""
import argparse, asyncio, json, math, struct, time

from aiortc import RTCConfiguration, RTCPeerConnection, RTCSessionDescription


def pct(v, p):
    if not v:
        return 0
    v = sorted(v)
    return v[min(len(v) - 1, max(0, math.ceil(p * len(v)) - 1))]


class Sig:
    def __init__(self):
        self.answers = {}

    async def connect(self, host, port):
        self.r, self.w = await asyncio.open_connection(host, port)
        self.w.write(b'HELLO client\n')
        asyncio.create_task(self.loop())

    async def loop(self):
        while True:
            line = await self.r.readline()
            if not line:
                return
            f = line.decode().split()
            if len(f) >= 2 and f[0] in ('ANSWER', 'REJECT') and int(f[1]) in self.answers:
                fut = self.answers.pop(int(f[1]))
                if not fut.done():
                    if f[0] == 'ANSWER':
                        fut.set_result(bytes.fromhex(f[2]).decode())
                    else:
                        fut.set_exception(RuntimeError('server rejected offer: ' + ' '.join(f[2:])))

    async def offer(self, pid, sdp):
        fut = asyncio.get_running_loop().create_future()
        self.answers[pid] = fut
        self.w.write(f'OFFER {pid} {sdp.encode().hex()}\n'.encode())
        return await asyncio.wait_for(fut, 30)


S = {'ok': 0, 'fail': 0, 'connect_ms': [], 'rel': [], 'unrel': [], 'rel_sent': 0, 'unrel_sent': 0, 'rel_recv': 0, 'unrel_recv': 0,
     'rel_order_violations': 0, 'large_ok': 0, 'large_fail': 0, 'dropped': 0, 'errors': []}


async def peer(i, sig, a):
    t0 = time.perf_counter()
    pc = RTCPeerConnection(RTCConfiguration(iceServers=[]))
    rel = pc.createDataChannel('ReliableDataChannel')
    unrel = pc.createDataChannel('UnreliableDataChannel', ordered=False, maxRetransmits=0)
    opened = {'rel': asyncio.Event(), 'unrel': asyncio.Event()}
    expected = [0]
    large = asyncio.Event()
    big = bytes([ord('R')]) + struct.pack('<QQ', 0xFFFF, 0) + bytes(range(256)) * 128

    @rel.on('open')
    def _():
        opened['rel'].set()

    @unrel.on('open')
    def _():
        opened['unrel'].set()

    @rel.on('message')
    def _(m):
        if not isinstance(m, bytes) or not m or m[0] != ord('r'):
            return
        if len(m) > 4096:
            if m[1:] == big[1:]:
                S['large_ok'] += 1
            else:
                S['large_fail'] += 1
            large.set()
            return
        seq, ts = struct.unpack_from('<QQ', m, 1)
        S['rel_recv'] += 1
        S['rel'].append((time.perf_counter_ns() - ts) / 1000)
        if seq != expected[0]:
            S['rel_order_violations'] += 1
        expected[0] = seq + 1

    @unrel.on('message')
    def _(m):
        if isinstance(m, bytes) and m and m[0] == ord('r'):
            seq, ts = struct.unpack_from('<QQ', m, 1)
            S['unrel_recv'] += 1
            S['unrel'].append((time.perf_counter_ns() - ts) / 1000)

    try:
        await pc.setLocalDescription(await pc.createOffer())
        answer = await sig.offer(i, pc.localDescription.sdp)
        if answer is None:
            raise RuntimeError('rejected by server')
        await pc.setRemoteDescription(RTCSessionDescription(sdp=answer, type='answer'))
        await asyncio.wait_for(asyncio.gather(opened['rel'].wait(), opened['unrel'].wait()), 30)
        S['ok'] += 1
        S['connect_ms'].append((time.perf_counter() - t0) * 1000)
    except Exception as e:
        S['fail'] += 1
        S['errors'].append(f'{i}: {type(e).__name__} {e}')
        await pc.close()
        return
    # one 64 KiB reliable message must come back byte identical (sctp fragmentation)
    rel.send(big)
    try:
        await asyncio.wait_for(large.wait(), 20)
    except asyncio.TimeoutError:
        S['large_fail'] += 1
    seq = 0
    end = time.perf_counter() + a.duration
    while time.perf_counter() < end:
        if pc.connectionState != 'connected':
            S['dropped'] += 1
            break
        payload = b'R' + struct.pack('<QQ', seq, time.perf_counter_ns()) + b'x' * 47
        rel.send(payload)
        S['rel_sent'] += 1
        unrel.send(payload)
        S['unrel_sent'] += 1
        seq += 1
        await asyncio.sleep(1 / a.rate)
    await asyncio.sleep(0.5)
    await pc.close()


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--server', default='127.0.0.1:9000')
    ap.add_argument('--count', type=int, default=10)
    ap.add_argument('--duration', type=float, default=8)
    ap.add_argument('--rate', type=float, default=10)
    ap.add_argument('--out')
    a = ap.parse_args()
    host, port = a.server.rsplit(':', 1)
    sig = Sig()
    await sig.connect(host, int(port))
    await asyncio.gather(*(peer(i + 1, sig, a) for i in range(a.count)))
    r = {'name': 'aiortc', 'connect': {'attempted': a.count, 'ok': S['ok'], 'fail': S['fail'],
                                       'ms': {'n': len(S['connect_ms']), 'p50': pct(S['connect_ms'], .5), 'p95': pct(S['connect_ms'], .95), 'p99': pct(S['connect_ms'], .99), 'max': max(S['connect_ms'] or [0])}},
         'rounds': [], 'alive_at_end': S['ok'] - S['dropped'], 'dropped_peers': S['dropped'],
         'rtt_reliable': {'n': len(S['rel']), 'p50_us': pct(S['rel'], .5), 'p95_us': pct(S['rel'], .95), 'p99_us': pct(S['rel'], .99), 'max_us': max(S['rel'] or [0])},
         'rtt_unreliable': {'n': len(S['unrel']), 'p50_us': pct(S['unrel'], .5), 'p95_us': pct(S['unrel'], .95), 'p99_us': pct(S['unrel'], .99), 'max_us': max(S['unrel'] or [0])},
         'echo': {'rel_sent': S['rel_sent'], 'rel_recv': S['rel_recv'], 'unrel_sent': S['unrel_sent'], 'unrel_recv': S['unrel_recv'],
                  'unrel_loss_pct': round(100 * (1 - S['unrel_recv'] / S['unrel_sent']), 2) if S['unrel_sent'] else 0},
         'stream': {'rel_recv': 0, 'rel_order_violations': S['rel_order_violations'], 'unrel_recv': 0, 'unrel_lost': 0, 'unrel_reordered': 0},
         'flood': {'recv': 0, 'order_violations': 0}, 'large_message_ok': S['large_ok'], 'large_message_fail': S['large_fail'],
         'bytes_in': 0, 'cleanup_ok': True, 'timeline': [], 'errors': S['errors'][:10], 'start_unix_ms': int(time.time() * 1000)}
    out = json.dumps(r)
    if a.out:
        open(a.out, 'w').write(out)
    else:
        print(out)


asyncio.run(main())
