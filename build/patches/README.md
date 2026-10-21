# Pinned libdatachannel distribution

The upstream version is 0.24.6, commit `6b1e2e620f1e37f0eafeee702eaea0043cb305fd`. This distribution adds the ordered patches in `../dependencies.lock.json`:

- `backend.patch`: the public message-lifetime send overload; Message deletion precedes release of the supplied owner
- `backend-identity.patch`: installed identity header, exported identity function and header installation entry

Both source modifications are MPL-2.0. Keep the upstream notices and `third-party/licenses/` when distributing build artifacts. Patch bytes are protected against newline conversion by `.gitattributes`; SHA-256 is checked before application.

`prepare-backend.cmake` verifies a clean upstream checkout and every locked submodule, then copies it into a new Git-free directory under ext-rtc's `.build`. Patches apply only to that copy. Existing destinations are rejected. Read-only Git checks disable optional index refresh, and patch application isolates Git discovery from the surrounding ext-rtc repository. The original checkout and global installations are not patched.

The Linux and Windows build scripts run this helper, preserve the locked feature/compiler options, compile/link the new overload and identity function, and execute the probe against the installed private library. Linux records the resolved library path and binary hashes; Windows additionally checks the loaded DLL path in its probe. Configure requires the actual exports, not just the version string. ext-rtc checks the expected identity again at module initialization and reports it in module information.

No existing virtual slot or class layout changes. Existing clients retain the old overloads; newly built ext-rtc requires both new exports. The public identity marker records build provenance, not protection against maliciously forged binaries. Keep header, library, compiler ABI and CRT configurations matched; native Windows validation is distinct from script syntax checks.
