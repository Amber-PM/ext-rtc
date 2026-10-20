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
- Automated native builds and tests through GitHub Actions.

## Compatibility

| Platform | PHP | Architecture |
|---|---|---|
| Linux | 8.1+ | x64, ZTS / NTS |
| Windows | 8.1+ | x64, ZTS |

PHP 8.1 and 8.4 builds have been validated through CI. Other versions and configurations may require additional validation.

## Development Status

**ext-rtc is under active development.**

The current native bootstrap exposes runtime information and capability detection. WebRTC connections and DataChannels are not yet available for production use.

## License

ext-rtc is licensed under the [MIT License](LICENSE).

Third-party dependencies retain their respective licenses. See `third-party/` for license notices.