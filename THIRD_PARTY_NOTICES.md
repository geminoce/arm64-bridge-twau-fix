# Third-party notices

The GPL-3.0-or-later license at the repository root applies to the bridge and control code. Bundled upstream components retain their own licenses, including all copyright and attribution notices in their files. Those public upstream attributions are deliberately preserved.

| Component | License | Bundled license |
| --- | --- | --- |
| Dynarmic 6.7.0 | Zero-Clause BSD (`0BSD`) | [LICENSE.txt](arm64_bridge/third_party/dynarmic/LICENSE.txt) |
| Boost 1.83.0 headers | Boost Software License 1.0 (`BSL-1.0`), subject to individual file notices | [LICENSE_1_0.txt](arm64_bridge/third_party/boost/LICENSE_1_0.txt) |
| fmt | MIT with the upstream compiled-code exception | [LICENSE](arm64_bridge/third_party/dynarmic/externals/fmt/LICENSE) |
| mcl | MIT | [LICENSE](arm64_bridge/third_party/dynarmic/externals/mcl/LICENSE) |
| oaknut | MIT | [LICENSE](arm64_bridge/third_party/dynarmic/externals/oaknut/LICENSE) |
| robin-map | MIT | [LICENSE](arm64_bridge/third_party/dynarmic/externals/robin-map/LICENSE) |

Individual source notices remain authoritative for any additional upstream code. The bundled Dynarmic source includes local modifications; the corresponding patches are recorded in `arm64_bridge/patches/dynarmic/` and are already applied.

No original game code or assets are licensed or supplied by these notices. Game API names and symbols in the bridge describe interoperability requirements.
