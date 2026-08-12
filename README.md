# iPXE-Stateless

[![License](https://img.shields.io/badge/License-GPL--2.0-green)](LICENSE)
[![GitHub stars](https://img.shields.io/github/stars/dutyc/ipxe-stateless)](https://github.com/dutyc/ipxe-stateless)
[![Version](https://img.shields.io/github/v/tag/dutyc/ipxe-stateless)](https://github.com/dutyc/ipxe-stateless/releases)
[![Platform](https://img.shields.io/badge/Platform-x86_64%20UEFI%2FBIOS-0f766e)](docs/network-support.md)
[![Upstream](https://img.shields.io/badge/Upstream-iPXE%20e6e51ccb-111111)](patches/README.md)
[![Patches](https://img.shields.io/badge/Patches-4-7c3aed)](docs/customizations.md)

[English](README.md) | [中文](README.zh-CN.md)

**Stateless cloud-native iPXE** — an automated firmware build repository based on **upstream iPXE + patch-based customization**.

iPXE-Stateless provides unified network boot firmware for stateless cloud-native environments: clients keep no state, obtaining configuration via DHCP at boot, chainloading boot scripts, and entering diskless systems. The repository itself is likewise stateless: it contains no iPXE source code, only difference patches and build assets, and can be rebuilt from any upstream baseline at any time.

## Related Projects

- **[iPXE-All-Ready](https://github.com/dutyc/ipxe-all-ready)** — a complete stateless cloud-native diskless compute platform: central Controller (FastAPI control plane + DHCP/TFTP/HTTP boot services), iSCSI Server nodes (stgt / LIO), diskless Worker nodes, Web UI and documentation site.

This repository is the **firmware foundation** of iPXE-All-Ready. Its boot-media guide consumes the firmware artifacts built here (`dist/`, RTL8125 driver + EMBED auto-boot editions) to bring Workers into the iPXE boot chain; the platform then takes over via DHCP-served scripts and iSCSI system disks. The two repositories have strictly separated responsibilities — **ipxe-stateless owns how the firmware is built; iPXE-All-Ready owns how the platform runs.**

## Features

- **Patch-based, no fork** — all modifications to upstream iPXE are maintained as `git apply`-able patches under `patches/`, generated against a fixed upstream baseline (`e6e51ccb`). Upgrading upstream means regenerating the patches, not merging a fork.
- **Native NIC driver adaptation** — RTL8125 (2.5G) and RTL8126 (5G) series patches provide reliable native-driver network boot (see [docs/customizations.md](docs/customizations.md)).
- **Local boot fallback** — SNP firmware adaptation for booting from local media (USB / disk / GRUB2 chainload) when the mainboard lacks a PXE boot option.
- **EMBED auto-boot script** — `embed/auto.ipxe` is compiled into the firmware; plug in and boot, no manual configuration.
- **Reproducible one-command build** — the pipeline fetches source, applies patches, builds, and archives 10 firmware artifacts together with `SHA256SUMS`.

## Quick Start

```bash
# Full build (fetch source -> apply patches -> build -> archive)
./build/build.sh

# Common variables
UPSTREAM_COMMIT=<sha> ./build/build.sh    # Pin upstream baseline (default e6e51ccb)
JOBS=8 ./build/build.sh                   # Parallelism (default: nproc)
UPSTREAM_URL=<mirror-url> ./build/build.sh # Alternate source mirror
```

Requirements: Linux with `git` / `make` / `gcc`, and network access to the upstream repository (GitHub by default, mirrors supported).

## Usage

All artifacts are written to `dist/`:

| Artifact | Form factor | Description |
|---|---|---|
| `dist/pxe-uefi/ipxe.efi` | PXE boot (UEFI) | No EMBED; boot script served via DHCP |
| `dist/pxe-uefi/ipxe-debug.efi` | Same (debug build) | `DEBUG=realtek:3`, for fault diagnosis |
| `dist/pxe-uefi/snponly.efi` | PXE boot (SNP-only, UEFI) | No EMBED; with firmware PXE chainloading takes over only the chainloaded device; falls back to all SNP devices when chainload location fails |
| `dist/pxe-uefi/snponly-debug.efi` | Same (debug build) | `DEBUG=realtek:3`, for fault diagnosis |
| `dist/direct-uefi/ipxe.efi` | UEFI direct boot (EMBED) | Embeds `auto.ipxe`; boot-and-go boot chain |
| `dist/direct-uefi/ipxe-debug.efi` | Same (debug build) | `DEBUG=realtek:3`, for fault diagnosis |
| `dist/direct-uefi/snponly.efi` | UEFI direct boot (SNP-only, EMBED) | Uses firmware SNP protocol; fallback path for machines where native drivers fail |
| `dist/grub-bios/ipxe.lkrn` | GRUB2 BIOS boot (EMBED) | `linux16 /ipxe.lkrn` |
| `dist/undionly.kpxe` | PXE boot (BIOS) | No EMBED; no native drivers, uses the UNDI interface of the NIC PXE ROM; compatible with any NIC that has a PXE ROM |
| `dist/usb/ipxe.usb` | BIOS boot media (EMBED) | Write whole-disk to a USB stick |

`dist/SHA256SUMS` lists the sha256 checksums of all artifacts.

## Documentation

| Document | Purpose |
|---|---|
| [docs/customizations.md](docs/customizations.md) | Design rationale and detailed description of every customization (patches 0001-0004 + EMBED) |
| [docs/network-support.md](docs/network-support.md) | NIC support matrix (well-covered / unsupported / conditional + field-test records) |
| [patches/README.md](patches/README.md) | Patch set details, licensing, and upstream baseline upgrade workflow |
| [docs/8168-research-log.md](docs/8168-research-log.md) | RTL8168 research log (investigation terminated) |

## Project Structure

```
ipxe-stateless/
├── patches/                 # Source patch set (applied in filename order)
├── embed/
│   └── auto.ipxe            # EMBED auto-boot script
├── build/
│   └── build.sh             # Automated build pipeline
├── docs/                    # Design, NIC support, research documents
├── reference/               # Reference source snapshots with patches applied (not used for builds)
├── dist/                    # Build artifacts (gitignored)
└── README.md
```

The build cache lives in `.cache/` (deletable and rebuildable without affecting the repository).

## How It Works

1. **Fetch source** — shallow-clone upstream `ipxe/ipxe`, check out the pinned baseline, `clean` before each build to guarantee a pristine tree.
2. **Apply patches** — `git apply --check` then apply in filename order; abort immediately if any patch fails.
3. **Install assets** — copy `embed/auto.ipxe` to the source `src/embed/`.
4. **Build** — build each artifact in the manifest (force-delete same-name EFI targets first so the EMBED parameter takes effect).
5. **Archive** — copy artifacts to `dist/` by category and generate `SHA256SUMS`.

## Upstream & License

The firmware is built from upstream **iPXE** ([github.com/ipxe/ipxe](https://github.com/ipxe/ipxe), GPL-2.0-or-later / UBDL dual-licensed). This repository contains no iPXE source code; all modifications are maintained as patches against the pinned baseline `e6e51ccb` (see [patches/README.md](patches/README.md)).

This repository as a whole is licensed under **GPL-2.0** (see [LICENSE](LICENSE)), compatible with upstream GPL-2.0-or-later / UBDL. Parts of the adaptations derived from third-party drivers are **GPL-2.0 only** and may not be redistributed under UBDL or any later licence (see the header declarations of the individual patches and [docs/customizations.md](docs/customizations.md)).
