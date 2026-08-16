# Build Artifacts

[English](build-artifacts.md) | [中文](build-artifacts.zh-CN.md)

The build pipeline (`build.sh`) compiles the firmware into 10 form factors, categorized and written to `dist/`:

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

## Checksums

`dist/SHA256SUMS` lists the SHA-256 checksums of all artifacts; verify with `sha256sum -c SHA256SUMS` inside `dist/`.

## Choosing an Artifact

- **PXE boot environments** (DHCP + boot server): `pxe-uefi/` for UEFI clients, `undionly.kpxe` for BIOS clients.
- **Direct / embedded boot** (boot-and-go, no DHCP script): `direct-uefi/` (UEFI), `grub-bios/ipxe.lkrn` (GRUB2 BIOS), `usb/ipxe.usb` (USB media).
- **Native-driver failures**: `snponly` variants fall back to the firmware SNP / UNDI interface.
- **Fault diagnosis**: `-debug` variants enable `DEBUG=realtek:3`.
