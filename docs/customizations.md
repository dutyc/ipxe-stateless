# Customizations

[English](customizations.md) | [中文](customizations.zh-CN.md)

Every customization in this repository, relative to the upstream iPXE baseline (default `e6e51ccb`), consists of **four patches** plus a **build-level EMBED customization** (`embed/auto.ipxe`, compiled into the firmware via `EMBED=`, not a patch).

| # | Patch | Scope |
|---|---|---|
| 0001 | `0001-realtek-8125-adaptation.patch` | RTL8125 (2.5G) series native driver adaptation |
| 0002 | `0002-makefile-ipxe-debug.patch` | Debug build fix |
| 0003 | `0003-snponly-local-boot.patch` | snponly local boot support |
| 0004 | `0004-realtek-8126-adaptation.patch` | RTL8126 (5G) native driver adaptation |

## Design Rationale

Many mainboards have no PXE boot option, or make it painful to use (BIOS without a Network Boot entry, UEFI network stack disabled by default, per-machine BIOS configuration with Secure Boot restrictions). Instead of depending on mainboard PXE, this repository provides firmware that boots from local media (USB / disk / GRUB2 chainload) and automatically enters the network boot flow — `embed/auto.ipxe` (EMBED customization) and `0003` (SNP local boot adaptation) exist for this purpose.

Customizations are maintained as patches rather than a fork: a fork branch needs continuous merging on every upstream upgrade, which is too costly. This repository instead uses:

```
upstream ipxe source (pinned baseline commit)
    +
patches/ (diff files, single source of truth)
    +
embed/ (script assets)
    ↓ build/build.sh
dist/ (ten firmware artifacts + SHA256SUMS)
```

All patches are generated against the **same pinned upstream baseline**; upgrading upstream means regenerating the patches (see [patches/README.md](../patches/README.md)).

## Customization Details

### 1. RTL8125 series adaptation (`0001`)

- **Rationale**: RTL8125 (2.5G) NICs must be handled by the iPXE native driver — the firmware SNP driver hangs in iSCSI mount scenarios and cannot be used for diskless boot; upstream iPXE has incomplete support for some 8125 versions (XID 0x688 series).
- **Changes**: `src/drivers/net/realtek.c`, `realtek.h`
  - XID 0x688 version table and device identification
  - EPHY initialisation table (2.5G PHY configuration)
  - 32-bit interrupt status register
  - FETCH / PAUSE_SLOT configuration
  - BAR 0x4808 (2.5G-specific register window)
  - TPPOLL_8125 polling
- **Licence**: the 8125 adaptation is derived from the Linux kernel r8169 driver (GPL-2.0-only); it is licensed under GPL-2.0 only and may not be redistributed under UBDL (see the header of `patches/0001`).

### 2. Debug build fix (`0002`)

- **Rationale**: the `ipxe-debug.efi` target had no driver set defined, so the artifact was an empty shell (no NIC drivers at all) and useless for fault diagnosis.
- **Changes**: `src/Makefile` — added `DRIVERS_ipxe-debug += $(DRIVERS_ipxe)` so the debug target inherits the full driver set.

### 3. snponly local boot support (`0003`)

- **Rationale**: the official `snponly.efi` only supports firmware-PXE chainload scenarios (it takes over only the device that loaded iPXE); booting from local UEFI (USB / disk) finds no NIC and drops straight to the shell. This is the companion adaptation for the "mainboard has no PXE boot option" scenario — local-media boot also needs network takeover capability.
- **Changes**: `src/drivers/net/efi/snponly.c` — when chainload location fails, fall back to taking over all SNP/NII/MNP devices; PXE chainload behaviour is unchanged.
- **Effect**: machines where native drivers are unavailable (e.g. RTL8168 initialisation hangs on certain mainboards) still have an SNP fallback path for local boot.

### 4. RTL8126 5GbE adaptation (`0004`)

- **Rationale**: RTL8126 (5G) is a 2024 NIC with no `0x8126` device entry in the upstream iPXE baseline. Its GPHY must be initialised with one of three PHY configuration methods (static register tables) selected by ICVerID, and some PCIe configuration (ZRXDC timeout reporting, ASPM entry latency) requires the CSI mechanism to access extended configuration space — none of which the baseline driver has.
- **Changes**: `src/drivers/net/realtek.c`, `realtek.h`
  - `0x8126` device entry and `realtek_detect_8126` (TxConfig 0x64800000 family detection + ICVerID → mcfg 1/2/3 dispatch)
  - GPHY OCP interface functions (`realtek_gphy_ocp_read/write/modify`, refactored from the 0001 inlined MII access and reused)
  - CSI extended configuration space interface (`realtek_csi_read/write/modify`, same mechanism as Linux `rtl_csi_*`)
  - PHY static configuration tables x3 (`realtek_8126a_1/2/3_phy`, 367 entries in total, from the official r8126 driver; MCU microcode section excluded; direct-write vs read-modify-write semantics distinguished, corresponding to the official `rtl8126_mdio_direct_write_phy_ocp` and `rtl8126_clear_and_set_eth_phy_ocp_bit` respectively) and `realtek_hw_phy_config_8126`
  - `realtek_hw_start_8126`: ZRXDC disabled + default ASPM entry latency (CSI path) + 8125 common initialisation + PHY configuration
  - Mount points: `realtek_detect` / `realtek_open` / `realtek_probe` dispatch on `mac_ver == 70`
- **Verification**: full build passes all 10 artifacts (including the `DEBUG=realtek:3` debug target); field testing on physical hardware pending.
- **Audit**: the dual-source PHY table audit and the lightweight PHY MCU firmware version-check policy are documented in [8126-porting-audit.md](8126-porting-audit.md) (Chinese only).
- **Licence**: the 8126 adaptation is derived from the Realtek r8126 driver (GPL-2.0-only, Copyright 2025 Realtek Semiconductor Corp.) and the Linux kernel r8169 driver (GPL-2.0-only); it is licensed under GPL-2.0 only and may not be redistributed under UBDL (see the header of `patches/0004`).

## EMBED Auto-Boot Script

`embed/auto.ipxe` is a configuration asset (not a source patch): changes do not require regenerating patches — modify the file and rebuild. See [patches/README.md](../patches/README.md) for details.
