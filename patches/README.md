# Patch Set

[English](README.md) | [中文](README.zh-CN.md)

All patches are generated against the **same upstream baseline**; apply order = filename order.

## Baseline

- Upstream repository: `https://github.com/ipxe/ipxe.git` (mirror available: `https://gitee.com/mirrors/ipxe.git`)
- Baseline commit: `e6e51ccbf17ff40a899c8859fb4e95abd5cfcd57` (master)
- To regenerate patches, in the build cache worktree (`.cache/ipxe-upstream`, which already contains the patches and modifications):

  ```bash
  git -C .cache/ipxe-upstream diff > patches/NNNN-xxx.patch
  ```

## Patch List

| Patch | Files | Content |
|---|---|---|
| `0001-realtek-8125-adaptation.patch` | `src/drivers/net/realtek.c`, `realtek.h` | RTL8125 series adaptation (XID 0x688 version table, EPHY initialisation table, 32-bit interrupt register, FETCH/PAUSE_SLOT, BAR 0x4808, TPPOLL_8125) |
| `0002-makefile-ipxe-debug.patch` | `src/Makefile` | Added `DRIVERS_ipxe-debug` definition (debug target inherits the full driver set, fixes `obj_ipxe_debug` link failure) |
| `0003-snponly-local-boot.patch` | `src/drivers/net/efi/snponly.c` | snponly local boot support: when chainload location fails (local UEFI boot), fall back to taking over all SNP/NII/MNP devices; PXE chainload behaviour unchanged |
| `0004-realtek-8126-adaptation.patch` | `src/drivers/net/realtek.c`, `realtek.h` | RTL8126 5GbE adaptation (ICVerID detection and PHY configuration dispatch, GPHY OCP/CSI interfaces, 3 PHY static configuration tables, ZRXDC/ASPM configuration) |
| `0005-device-info-collection.patch` | `src/include/ipxe/smbios.h`, `src/interface/smbios/smbios_settings.c`, `src/drivers/bus/pci.c` | Device information collection: SMBIOS type 17 memory settings (`mem-total` aggregated over all modules, `mem-type` / `mem-speed` from first module, custom fetches) + PCI `driver_name` population so `${net0/chip}` reports the device-table name (e.g. `RTL8125`) |
| `0006-nvmeof-adaptation.patch` | `src/config/config.c`, `src/config/general.h`, `src/include/ipxe/errfile.h`, `src/include/ipxe/nvmetcp.h`, `src/net/tcp/nvmetcp.c`, `src/net/tcp/nvmetcp_auth.c`, `src/tests/nvmetcp_test.c`, `src/tests/tests.c` | NVMe-oF (NVMe over TCP) SAN protocol support: new nvmetcp driver (ICReq/ICResp parameter negotiation, Property Set (CC register), Connect/Identify, block read/write, R2T flow control, DH-HMAC-CHAP authentication, EFI device-path description and BlockIo hooking) + `SANBOOT_PROTO_NVME_TCP` config and unit tests |
| `0007-nvmetcp-auth-fix.patch` | `src/include/ipxe/nvmetcp.h`, `src/net/tcp/nvmetcp.c`, `src/net/tcp/nvmetcp_auth.c`, `src/tests/nvmetcp_test.c` | NVMe/TCP authentication and state-machine fixes: transient `-EAGAIN` handling (wait for window, do not close the session), idempotent AuthReceive send with unified step advancement (no duplicate AuthReceive), authentication phase completion gated by `completed`/`rx_complete` flags plus command-id matching (`rx_cid`) so the Property Set is never skipped (fixes Identify rejected with `0x8018`), Identify Namespace LBAF offset fix (64→128), `NVME_SC_AUTH_REQUIRED` constant, authentication state-machine unit tests (phase completion gating, AuthReceive command-id matching, START-step guard). See `../docs/nvmeof-auth-debug-log.md` |

> RTL8168 research was terminated in 2026-08; the patches contain no 8168 filtering or fix code (see `../docs/8168-research-log.md`).

## Licensing

- The upstream files modified by the patches (`realtek.c`, `snponly.c`, `Makefile`, `smbios.h`, `smbios_settings.c`, `pci.c`) inherit the iPXE GPL-2.0-or-later / UBDL licence;
- the RTL8125 adaptation parts in `0001` (XID version table, EPHY initialisation, power management, etc.) are derived from the Linux kernel r8169 driver (`drivers/net/ethernet/realtek/r8169_main.c`, GPL-2.0-only); those parts are **GPL-2.0 only** and may not be redistributed under UBDL or any later licence;
- the RTL8126 adaptation parts in `0004` (PHY static configuration tables, GPHY OCP/CSI interfaces, ZRXDC/ASPM configuration, etc.) are derived from the Realtek r8126 driver (`r8126_n.c`, GPL-2.0-only, Copyright 2025 Realtek Semiconductor Corp.) and the Linux kernel r8169 driver (`drivers/net/ethernet/realtek/r8169_main.c`, GPL-2.0-only); those parts are **GPL-2.0 only** and may not be redistributed under UBDL or any later licence;
- the new NVMe/TCP driver files in `0006`/`0007` (`nvmetcp.c`, `nvmetcp_auth.c`, `nvmetcp.h`, `nvmetcp_test.c`) are original ipxe-stateless implementations carrying `FILE_LICENCE ( GPL2_ONLY )`; they are **GPL-2.0 only** and may not be redistributed under UBDL;
- this repository as a whole is licensed under GPL-2.0 (see `../LICENSE`); every patch header carries an SPDX declaration.

## Upgrading the Upstream Baseline

After an upstream upgrade the patches may no longer apply. Migration workflow:

1. Update the baseline:

   ```bash
   git fetch https://github.com/ipxe/ipxe.git master
   git log FETCH_HEAD --oneline | head   # confirm the new baseline
   ```

2. Regenerate the patches in the build cache worktree (`.cache/ipxe-upstream`) against the new baseline:

   ```bash
   # In .cache/ipxe-upstream: check out the new baseline, reproduce the modifications
   # (or apply the old patches and resolve conflicts), then:
   git diff > patches/0001-realtek-8125-adaptation.patch
   ```

   Note: **apply the old patches to the new baseline → resolve conflicts manually → regenerate** is more reliable than rewriting by hand; regenerating overwrites the header licence comment, so copy it from the old patch header (see "Licensing" above).
3. Update `UPSTREAM_COMMIT` in `build/build.sh` and the baseline records in this document.

4. Run `./build/build.sh` to verify the patches apply cleanly and the artifacts work (key regression: 8125/8126 boot).

## Notes

- `embed/auto.ipxe` is a **configuration asset** (not a source patch): changes do not require regenerating patches — modify `../embed/auto.ipxe` and rebuild.
- Patches must pass `git apply --check`; patches are strongly tied to the baseline commit — always go through the upgrade workflow before changing `UPSTREAM_COMMIT`.
