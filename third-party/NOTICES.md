# Dependency inventory and license notices

| Component | Revision/version | Role | License |
| --- | --- | --- | --- |
| libdatachannel | 6b1e2e620f1e37f0eafeee702eaea0043cb305fd (0.24.6) | shared backend | MPL-2.0 |
| libjuice | b89c792e3612faf2f12cf35bcc56857313a06be3 | static ICE backend dependency | MPL-2.0 |
| usrsctp | fec583d54493f879d2ae44a743423bf8a04371ab | static SCTP backend dependency | BSD-3-Clause |
| plog | 94899e0b926ac1b0f4750bfbd495167b4a6ae9ef | backend logging headers | MIT |
| OpenSSL | 3.5.5; upstream source 67b5686b4419b4cb8caa502711c41815f5279751 | shared TLS/crypto dependency | Apache-2.0 |
| json | 55f93686c01528224f448c19128836e7df245f72 | recorded submodule, WebSocket disabled | MIT |
| libsrtp | 24b3bf8f19b6f5ab4cd2bcceb4f4064efca86fd5 | recorded but not built, media disabled | BSD-3-Clause |

The distributed libdatachannel is **0.24.6 + ext-rtc message-lifetime patch**,
not unmodified upstream. `build/dependencies.lock.json` records its upstream
commit and SHA-256 for `build/patches/backend.patch` and the separate additive
`backend-identity.patch`. Both backend modifications are MPL-2.0. The packaging
helpers and capability probe are MIT. Distributions retain these patches,
this notice, and the upstream license files so the modified source can be
reconstructed from the pinned public repository and its locked submodules.
