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

## Development Status

**ext-rtc is under active development.**

## License

ext-rtc is licensed under the [MIT License](LICENSE).

Third-party dependencies retain their respective licenses. See `third-party/` for license notices.
