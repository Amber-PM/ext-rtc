<h1 align="center">
  <a href="https://github.com/Amber-PM/ext-rtc">
    <img src=".github/readme/amberpm.png" width="128" height="128" alt="AmberPM Logo" title="AmberPM" />
  </a>
</h1>

<p align="center">
  <strong>Native WebRTC DataChannels for PHP</strong>
  <br />
  Powered by <a href="https://github.com/paullouisageneau/libdatachannel">libdatachannel</a>
</p>

---

## Features

- Native C++20 implementation.
- Experimental WebRTC DataChannels with reliable and unreliable transport.
- Native build and test targets for Linux and Windows.
- Runtime information, capability detection and statistics.
- Experimental native PeerConnection answerer with SDP, ICE and observed DTLS fingerprints.
- Automated native builds and tests through GitHub Actions.

## Compatibility

| Platform | PHP | Architecture |
|---|---|---|
| Linux | 8.4.27, 8.5.12 | x64, ZTS / NTS |
| Windows | 8.4.27, 8.5.12 | x64, ZTS |

The build matrix targets PHP 8.4.27 (old stable) and PHP 8.5.12 (current stable), both from the 2026-10-21 security releases. The previous PHP 8.1.34 and 8.4.24 targets are retired and no longer built or tested. Other versions and configurations require additional validation.

## Sending, memory and backpressure

- Each peer queues up to 256 messages and `maxSendQueueSize` bytes. `DataChannel::send()` returns `false` when a queue is full or the shared pool is short, the message is not queued and
  nothing is dropped, the caller retries. Reliable data is never discarded silently.
- Queued sends share a pool of 48 MiB inside the 64 MiB managed budget. Above 31 KiB a queue keeps to an equal part of it, as if at least 64 peers were connected. A peer that had more
  than 4 MiB accepted by its association, which only happens when the remote acknowledges, also uses the last 12 MiB, so peers that stall right after connecting cannot starve established ones.
- ext-rtc never closes a peer for being slow. A peer whose remote stopped answering keeps its queued message until the backend gives the association up, about 13 s later.
- The SCTP association keeps its own 1 MiB send buffer outside the managed budget. A peer that does not acknowledge fills it, measured 0.9 MiB of process memory per frozen peer.
  Size the host for the managed pool plus about 1 MiB per connected peer in that worst case.
- Two workers send at a time. A worker that stays inside a backend call for more than 25 ms stops counting and another one takes its place, so peers that block in the backend do not hold up
  the others. Threads are only created after such a stall and never exceed the number of connected peers plus two, with a ceiling of 1024.

## Development Status

**ext-rtc is under active development.**

## License

ext-rtc is licensed under the [MIT License](LICENSE).

Third-party dependencies retain their respective licenses. See `third-party/` for license notices.
