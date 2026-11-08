# ext-rtc stress, benchmark and interop harness

Linux only (needs `/proc`). Everything is localhost.

| file | role |
|---|---|
| `server.php` | PHP answerer driver on ext-rtc, mux (single UDP port) or individual ports. Echo, numbered streams, floods and server side counters/samples |
| `client.cpp` | libdatachannel offerer host, N peers per process, RTT histogram with per second timeline |
| `stress.py` | orchestrator for the scenarios, writes JSON per run and `summary.md` |
| `interop/aiortc_client.py` | independent WebRTC stack (aiortc) against the same server |
| `run-all.sh` | full matrix for one module build |

## run

```sh
# client against the pinned patched libdatachannel prefix (same one the extension links), built into .build/stress which git ignores
cmake -S tests/stress -B .build/stress -DCMAKE_PREFIX_PATH=<prefix> -G Ninja && cmake --build .build/stress
pip install --target .build/stress/pylib aiortc        # only for the interop scenario (or set RTC_STRESS_PYLIB / --pylib)
tests/stress/run-all.sh <path/to/rtc.so> <outdir> 3
# single scenarios
python3 tests/stress/stress.py --module rtc.so --client .build/stress/rtc-stress-client --out out --repeat 3 connect-300-mux
python3 tests/stress/stress.py ... --soak-seconds 14400 soak-100-mux      # hours long soak
python3 tests/stress/stress.py ... --pylib <pylib> interop-10-mux interop-10-ports
```

`run-all.sh` builds the client by itself when `RTC_STRESS_CLIENT` is not set and `.build/stress/rtc-stress-client` is missing, using `RTC_PREFIX` or the first `.build/*/install`.

## pass or fail

The harness is a gate: `stress.py` exits 0 only when every run passes, 1 when any run fails and 2 for a bad command line or a missing module or client.
Each run writes `<outdir>/<scenario>.run<k>.json` with `verdict`, `failures`, `tolerated` and `warnings`, and the whole invocation writes `verdict.json` and `summary.md`.
`run-all.sh` writes the connect matrix to `<outdir>/connect` and the other scenarios to `<outdir>/scenarios`, stops after the first failing group (`--keep-going` runs the rest)
and exits 1 when any group failed. Every run is executed unless `--fail-fast` is given, all children are killed after each run.

| criterion | fails when |
|---|---|
| server | no `FINAL` stats (crash, hang, killed after `--quit-timeout`), exit code not 0 after QUIT, control connection lost or `STATS` slower than `--stats-timeout` |
| clients | timeout, crash without JSON, nonzero exit code, `cleanup_ok` false, `errors`, failed large messages |
| connections | failed handshakes above `--max-connect-fail-pct` (default 0, strict), nothing connected, peers dropped by the server (except the frozen peers of `slow`) |
| leaks | `activePeers`, `activeChannels` or `managedBufferBytes` not 0 at the end, drain timeout, threads or fds at the end more than `--thread-slack` above the lowest sample, soak fd or thread slope above `--max-slope-per-min` |
| messages | reliable order violations (numbered streams and floods) above 0, reliable echo unanswered above `--max-rel-unanswered-pct`, unreliable echo or stream loss above `--max-unrel-loss-pct`, receive overflow, receive/poll/echo exceptions, bad channels, `recv_rel_bad` |
| scenarios | `abrupt`: the victims did not all connect (`--abrupt-connect-wait`), the server held fewer peers than expected when they were killed, or it never released the killed clients. `slow`/`slowdelay`: the fast peers made no progress while the others were frozen |
| latency | reliable echo round trip p95 or p99 above the limit of the scenario, see below. `--no-latency-limits` switches it off, `--latency-scale` adapts it to a slower machine |
| nothing ran | `--repeat 0` is a usage error, a gate that runs nothing proves nothing |

Rejections (`send()` false, `BUFFER_BUDGET`, native `sendRejectedCount`) are reported and never fail a run by themselves, they are the expected backpressure.
An echo that meets a peer the client closed in the same moment throws `RtcException::CLOSED`, a race no server can avoid when peers close with traffic in flight.
It is counted as `echo_closed_races` and only warned about, every other exception code fails the run.
`--max-rss-slope-kb-min` makes the soak RSS slope a failure, without it growth above 300 KB/min is only a warning because it cannot tell a leak from fragmentation.

Raw libdatachannel has a known DTLS handshake flake of roughly 1 in 2500 to 6000 connections. The default is strict so it shows up as a failure. A CI gate may pass
`--max-connect-fail-pct 0.5`: runs inside the tolerance get the verdict `PASS_WITH_TOLERATED` and the console, `summary.md` and `verdict.json` list the failed handshakes.
That tolerance is only for handshakes, a lost message or a leak is never tolerated.

## latency limits

`LATENCY_LIMITS_MS` in `stress.py` holds a (p95, p99) limit per canonical scenario `kind-N-mode`, a scenario typed as `connect-100` is checked as `connect-100-mux`.
Every limit is three times the worst value recorded in `latency-baseline.json`: 3 to 8 runs of the original module on an idle machine, `calibrate.py export` writes
that file from result directories and `calibrate.py table` prints the tables. `calibrate.py check` fails when the tables and the recorded runs disagree, when a scenario has fewer
than three recorded runs or when a scenario of `run-all.sh` has no limit. The margin is 3 because healthy runs under a machine load of 1.6 to 3 reached 2.7 times the idle worst
value. What the limits must catch is far larger: the dispatcher with four concurrent senders measured p95 169 ms against the 28 ms limit of `slow-100-mux`.
A failure message includes the machine load, the limits assume an idle machine.

## self test

`tests/stress/selftest.py --module rtc.so --client .build/stress/rtc-stress-client --out <dir>` runs 41 small cases that inject faults around a real server and client
(a php wrapper that kills or freezes the server, exits with an error, or rewrites its final stats and samples; a client wrapper that rewrites the client JSON) and checks the
exit code and the reason in `verdict.json`, plus neutral controls that must pass, `run-all.sh` propagation and the consistency of the latency limits with the recorded runs.
Every check of `evaluate()` has a case: disabling a check in a copy of `stress.py` makes its case report `NOT DETECTED`, which was verified one by one. The faults prove that the harness detects each reported
condition, they do not prove that the extension produces it. `--legacy --stress <old stress.py>` runs the same cases against an older harness to show what it let through.

Scenario names are `kind-N-mode` with mode `mux` or `ports`:
`connect` (N concurrent connects plus echo RTT), `reconnect` (rounds of mass close and mass reconnect), `both` (simultaneous
reliable and unreliable bidirectional streams, 512 byte messages, 100 msg/s per channel), `abrupt` (half of the peers killed with SIGKILL while the
other half keeps echoing, then a fresh group reconnects), `slow` (a tenth of the peers receive a server flood and the client process is frozen with SIGSTOP),
`slowdelay` (same flood but the client callback sleeps 200 ms per message), `soak` (echo plus 10 percent churn every 10 s, RSS/fd/thread slopes), `interop`.

## protocol

Signaling is a line protocol on one TCP connection per client process: `HELLO client`, `OFFER <id> <hex sdp>`, answer `ANSWER <id> <hex>` or `REJECT <id> <reason>`.
A control connection (`HELLO ctl`) accepts `STATS` and `QUIT`. Data messages start with a type byte: `R` echo request answered by `r`, `D` numbered data,
`F` numbered flood, `CSTREAM rate size` and `CFLOOD size count` control. Ordering on the reliable channel is verified by sequence numbers,
loss on the unreliable one by sequence gaps.

## what is measured

Server side (every second into `<run>.samples.jsonl`): RSS, threads, fds, CPU user/sys, `RtcRuntime::getStats()` (active peers, channels, managed buffer bytes,
receive overflows, native send rejections), loop stall counters (`loop_max_ms`, iterations over 10/100 ms), create/close time, echo/stream/flood counters,
`send()` false returns and RtcException codes. Client side: connect success/fail and time, RTT p50/p95/p99/max per channel and per second, unreliable loss, dropped peers.
When PHP has FFI the server also samples the glibc allocator (`heap_used_kb`, `heap_free_kb`, `heap_arena_kb`) and, after the last peer is gone, records how much of the resident set
`malloc_trim` gives back (`trim` in the soak row): RSS growth with flat `heap_used_kb` is memory the allocator keeps, growth of `heap_used_kb` is a leak.
After the clients exit the harness waits until the server has `activePeers == 0` and records the drain time, `final_managed_bytes` must be 0.
The CPU of the PHP server includes its polling loop (`idle-us` 300). RTT is reported by 2 percent wide histogram buckets.
Client and server share the host, so absolute numbers depend on machine load (`loadavg` is recorded for each run).

## pending, not covered here

Real Minecraft Bedrock 26.60+ client: not available to this harness, nothing here simulates it. To validate on a real client, run the Amber server with the
rtc backend in mux mode, join with a retail client (PC and phone) and with a client one version behind, then check: join time, `/status` bandwidth, no
`channel_invalid`, repeated join/leave 50 times, kill the client app mid game and verify the slot is released, and a session of at least one hour while watching
`RtcRuntime::getStats()` and RSS. Windows runs (the libjuice mux stall) need a Windows build of the extension and are not covered by this Linux harness.
