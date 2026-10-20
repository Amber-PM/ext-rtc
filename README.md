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
- WebRTC DataChannel support *(in development)*.
- Reliable and unreliable data transport *(planned)*.
- Cross-platform support for Linux and Windows.
- Runtime information, capability detection and statistics.
- Experimental native PeerConnection answerer with SDP, ICE and observed DTLS fingerprints.
- Automated native builds and tests through GitHub Actions.

## Compatibility

| Platform | PHP | Architecture |
|---|---|---|
| Linux | 8.1+ | x64, ZTS / NTS |
| Windows | 8.1+ | x64, ZTS |

The build matrix targets PHP 8.1 and 8.4. Other versions and configurations require additional validation.

## Development Status

**ext-rtc is under active development.**

## License

ext-rtc is licensed under the [MIT License](LICENSE).

Third-party dependencies retain their respective licenses. See `third-party/` for license notices.
