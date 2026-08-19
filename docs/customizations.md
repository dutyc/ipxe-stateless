# Customizations

[English](customizations.md) | [中文](customizations.zh-CN.md)

Every customization in this repository, relative to the upstream iPXE baseline (default `e6e51ccb`), consists of **five patches** plus a **build-level EMBED customization** (`embed/auto.ipxe`, compiled into the firmware via `EMBED=`, not a patch).

| # | Patch | Scope |
|---|---|---|
| 0001 | `0001-realtek-8125-adaptation.patch` | RTL8125 (2.5G) series native driver adaptation |
| 0002 | `0002-makefile-ipxe-debug.patch` | Debug build fix |
| 0003 | `0003-snponly-local-boot.patch` | snponly local boot support |
| 0004 | `0004-realtek-8126-adaptation.patch` | RTL8126 (5G) native driver adaptation |
| 0005 | `0005-device-info-collection.patch` | Device info collection: SMBIOS type 17 memory settings (`mem-total` / `mem-type` / `mem-speed`) + PCI device-table name via `${net0/chip}` |

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

### 5. Device information collection (`0005`)

- **Rationale**: firmware-side device info collection (identity / CPU / memory / NIC) for HTTP reporting. Identity (SMBIOS types 1-3) and CPU (CPUID) settings are already provided upstream, leaving two gaps: no named settings for SMBIOS type 17 (memory devices, one structure per slot), and PCI NICs never populate `driver_name`, so `${net0/chip}` was unusable on PCI.
- **Changes**: `src/include/ipxe/smbios.h`, `src/interface/smbios/smbios_settings.c`, `src/drivers/bus/pci.c`
  - `struct smbios_memory_device` (type 17 layout verified against three dmidecode versions: Memory Type `0x12`, Speed `0x15`, Extended Size `0x1C`) + `SMBIOS_TYPE_MEMORY_DEVICE 17`
  - `${mem-total}` (uint32 MB, aggregated over all modules: `0xFFFF` skipped, `0x7FFF` falls back to Extended Size, bit 15 = GB), `${mem-type}` (first module, mapped to strings such as `DDR5`), `${mem-speed}` (first module, MT/s) — custom fetches dispatched by name, reusing the existing SMBIOS settings scope
  - `pci_probe` now sets `driver_name` from the matching device-table entry, enabling `${net0/chip}` (e.g. `RTL8125`) for all PCI NICs
- **Usage**: settings reference and report URL templates in [device-info-reporting.md](device-info-reporting.md) / [device-info-reporting.zh-CN.md](device-info-reporting.zh-CN.md).
- **Verification**: full build passes all 10 artifacts; settings embedded (strings check); behaviour on real hardware pending (URL encoding of spaces / special characters).

### 6. NVMe-oF (NVMe over TCP) SAN support (`0006`)

- **Rationale**: upstream iPXE has no NVMe-oF transport support; SAN boot from NVMe/TCP targets (e.g. Linux `nvmet`) requires a full protocol driver plus a `SANBOOT_PROTO_NVME_TCP` config option.
- **Changes**: `src/config/config.c`, `src/config/general.h` (`SANBOOT_PROTO_NVME_TCP`), `src/include/ipxe/errfile.h`, `src/include/ipxe/nvmetcp.h`, `src/net/tcp/nvmetcp.c`, `src/net/tcp/nvmetcp_auth.c`, `src/tests/nvmetcp_test.c`, `src/tests/tests.c`
  - 8-phase state machine: ICReq/ICResp parameter negotiation → Connect (Admin) → Property Set (CC.EN=1) → Identify (controller/namespace) → Connect (I/O) → block read/write with R2T flow control
  - DH-HMAC-CHAP authentication (AuthSend/AuthReceive, DH groups 0/2048/3072/4096)
  - EFI device-path description and BlockIo hooking for `sanboot`; unit tests (structure layout + Identify NS parsing)
- **Usage**: end-to-end guide (nvmet target setup incl. auth, `sanboot` syntax, QEMU validation) in [nvmeof-usage.md](nvmeof-usage.md) (Chinese only).
- **Verification**: QEMU/OVMF + Ubuntu 26.04 kernel 7.0 nvmet target, GRUB 2.14 SAN boot chain passing (see [nvmeof-auth-debug-log.md](nvmeof-auth-debug-log.md), Chinese only).
- **Licence**: original ipxe-stateless implementation, `FILE_LICENCE ( GPL2_ONLY )` — GPL-2.0 only, not redistributable under UBDL.

### 7. NVMe/TCP authentication and state-machine fixes (`0007`)

- **Rationale**: three bugs found while validating the 0006 auth path end-to-end against nvmet: transient `-EAGAIN` killed the session instead of waiting for the TCP window, AuthReceive could be sent twice, and the auth-complete race (phase switched before the final AuthReceive completion, so the Property Set was skipped and Identify was rejected).
- **Changes**: `src/include/ipxe/nvmetcp.h`, `src/net/tcp/nvmetcp.c`, `src/net/tcp/nvmetcp_auth.c`, `src/tests/nvmetcp_test.c`
  - Transient `-EAGAIN` handling (defer the process, resume on window) and idempotent AuthReceive send with unified step advancement
  - Auth phase completion gated by `completed`/`rx_complete` flags plus command-id matching (`rx_cid`), so the Property Set is never skipped (fixes Identify rejected with `0x8018`); `NVME_SC_AUTH_REQUIRED` status-only trigger for Connect (no ATR bit)
  - Identify NS LBAF offset fix (64→128)
- **Debug log**: full investigation timeline with wire-level evidence in [nvmeof-auth-debug-log.md](nvmeof-auth-debug-log.md) (Chinese only).
- **Verification**: re-run of the full chain: `sending Property Set (CC)` present, all commands `status=0x0000` on the wire, `0x8018` zero occurrences, GRUB 2.14 SAN boot OK.

## EMBED Auto-Boot Script

`embed/auto.ipxe` is a configuration asset (not a source patch): changes do not require regenerating patches — modify the file and rebuild. See [patches/README.md](../patches/README.md) for details.
