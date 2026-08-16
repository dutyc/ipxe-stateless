# iPXE-Stateless

[![License](https://img.shields.io/badge/License-GPL--2.0-green)](LICENSE)
[![GitHub stars](https://img.shields.io/github/stars/dutyc/ipxe-stateless)](https://github.com/dutyc/ipxe-stateless)
[![Version](https://img.shields.io/github/v/tag/dutyc/ipxe-stateless)](https://github.com/dutyc/ipxe-stateless/releases)
[![Platform](https://img.shields.io/badge/Platform-x86_64%20UEFI%2FBIOS-0f766e)](docs/network-support.md)
[![Upstream](https://img.shields.io/badge/Upstream-iPXE%20e6e51ccb-111111)](patches/README.md)
[![Patches](https://img.shields.io/badge/Patches-5-7c3aed)](docs/customizations.md)

[English](README.md) | [中文](README.zh-CN.md)

**iPXE-Stateless** is a **network boot firmware build repository** for stateless cloud-native computing environments: it contains no iPXE source code — only difference patches and build assets — rebuildable on any upstream baseline. It is the firmware foundation of the iPXE-All-Ready platform — the platform makes compute stateless; this repository makes the boot firmware stateless.

----

## Customizations

All modifications to upstream iPXE are maintained as `git apply`-able patches against the pinned baseline (`e6e51ccb`), currently five: native drivers for the RTL8125 (2.5G) and RTL8126 (5G) series, SNP local-boot fallback, realtek debug builds, and device information collection (SMBIOS memory + NIC chip name). Design rationale and implementation: **[docs/customizations.md](./docs/customizations.md)**. NIC support matrix and field-test records: [docs/network-support.md](./docs/network-support.md).

## Quick Start

```bash
./build/build.sh    # Full build: fetch source -> apply patches -> build -> archive
```

Requirements: Linux with `git` / `make` / `gcc`. Artifacts are written to `dist/` (10 form factors + `SHA256SUMS`); see [docs/build-artifacts.md](./docs/build-artifacts.md) for the full list and selection guide.

## Documentation

- [Customizations](./docs/customizations.md) — design rationale and implementation of every patch
- [NIC support matrix](./docs/network-support.md) — coverage and field-test records
- [Device information reporting](./docs/device-info-reporting.md) — collected settings and report URL usage
- [Build artifacts](./docs/build-artifacts.md) — artifact list, checksums, selection guide
- [Patch set](./patches/README.md) — licensing and upstream baseline upgrade workflow
- [RTL8126 porting audit](./docs/8126-porting-audit.md) — dual-source porting audit records (Chinese only)

## Platform Repository

**[iPXE-All-Ready](https://github.com/dutyc/ipxe-all-ready)** — the cloud-native platform that carries statelessness to the compute layer itself: compute is not bound to any specific hardware; nodes boot on plug-in, discardable and rebuildable in seconds. Two sides of the same idea: the platform makes compute stateless; this repository makes the boot firmware stateless.

## Community & Contribution

Welcome Star / Watch / Issues / Pull Requests. As with the platform repository: **AI may write the syntax; the architecture must be understood by humans**.

## License

Licensed under **[GPL-2.0](./LICENSE)**, compatible with upstream iPXE (GPL-2.0-or-later / UBDL dual-licensed). Adaptations derived from third-party drivers are **GPL-2.0 only** and may not be redistributed under UBDL or any later licence — see [patches/README.md](./patches/README.md).
