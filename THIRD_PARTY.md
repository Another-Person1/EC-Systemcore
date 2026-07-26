# Third-party software

ec-systemcore vendors the following dependency so builds do not silently change
when an upstream branch moves.

## Simple Open EtherCAT MainDevice (SOEM)

- Upstream: https://github.com/OpenEtherCATsociety/SOEM
- Declared upstream baseline: 2.0.0
- Upstream tag commit: `304d1c05eab77dc0d426f1a5cf09c8cc7dc03713`
- Incorporated Linux/runtime fixes through upstream commit:
  `7a8011bb28c0469bff2c9f838aacba2fa2df53e2`
- Repository snapshot commit: `d855b0c16f8b529070a88e59140d842d68438dbc`
- Local path: `third_party/SOEM`
- License: GPL-3.0-only or a separately purchased commercial license; see
  `third_party/SOEM/LICENSE.md`.

The vendored directory is a regular tracked fork, not a Git submodule. It is a
locally reorganized snapshot based on 2.0.0 with the applicable post-tag
Linux/runtime fixes through the commit named above; it is not claimed to be
byte-identical to either that tag or a later upstream `master`. Later
comments-only and Win32-only changes are not represented as target runtime
fixes. Updates must name the upstream boundary here, preserve the upstream
license, and review/document local differences independently from first-party
changes.

## Bun target runtime

- Upstream: https://github.com/oven-sh/bun
- Release: 1.3.14 (`bun-v1.3.14`)
- Asset: `bun-linux-aarch64.zip`
- Asset SHA-256:
  `a27ffb63a8310375836e0d6f668ae17fa8d8d18b88c37c821c65331973a19a3b`
- Extracted executable SHA-256: generated from that verified archive in the
  pinned image, then re-verified throughout CMake installation and packaging
- Installed path: `/usr/lib/ec-systemcore/runtime/bun`
- Installed provenance:
  `/usr/share/ec-systemcore/bun-runtime-provenance.json`
- License and linked-library notices: `licenses/BUN-LICENSE.md`

The official runtime is downloaded only while building the pinned toolchain
image, verified before extraction, re-verified by CMake, and byte-checked again
in the staged package. The package does not depend on an unverified target-feed
runtime.

## Build-only pinned tools

The reproducible CI toolchain also pins the host Bun tool, CMake, actionlint,
Syft, opkg-utils, and:

- Debian 12.11 Bookworm slim manifest:
  `sha256:b1a741487078b369e78119849663d7f1a5341ef2768798f7b7406c4240f86aef`
- Debian Bookworm package snapshot: `20260720T000000Z`
- Eclipse Temurin JDK 25.0.3+9 x64 archive:
  `OpenJDK25U-jdk_x64_linux_hotspot_25.0.3_9.tar.gz`
- JDK archive SHA-256:
  `69264a7a211bf5029830d07bc3370f879769d62ebc5b5488e90c9343a2da0e1f`

Exact versions, immutable commits, image digests, and release checksums are
recorded in
`.docker/linux-arm64.Dockerfile` and `.github/workflows/build-package.yml`.
