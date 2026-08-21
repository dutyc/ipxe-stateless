#!/usr/bin/env bash
# Run one QEMU round of the full NVMe/TCP SAN boot chain:
#   iPXE sanboot (NVMe/TCP) -> GRUB -> kernel -> initramfs NBFT consumer
#   (nbft_auto) -> nvme connect-all --nbft -> rootfs -> system boot
#
# The NBFT table is injected externally with -acpitable (the pre-iPXE
# phase; iPXE-side table generation is the follow-up implementation).
#
# Usage: bash test/run-qemu-nbft.sh [round]     (default round: "nbft")
#
# Prerequisites:
#   1. sudo bash test/nvmet-setup.sh          (target, AUTH=0)
#   2. bash test/make-debian-san-disk.sh      (2G SAN boot disk)
#   3. build the iPXE firmware and deploy it to the FAT drive:
#        make -C .cache/ipxe-upstream/src bin-x86_64-efi/ipxe.efi \
#          EMBED=test/nvmeof-test.ipxe DEBUG=nvmetcp,tcp,efi_block:3
#        cp .cache/ipxe-upstream/src/bin-x86_64-efi/ipxe.efi diag/tmp/EFI/BOOT/BOOTX64.EFI
#   4. python3 test/gen-nbft-table.py         (diag/nbft-qemu.bin)
#
# Outputs: diag/qemu-<round>.log, diag/netdump-<round>.pcap
# Run outside sandbox (requires /dev/kvm).
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
D=$ROOT/diag
ROUND=${1:-nbft}
LOG=$D/qemu-$ROUND.log
PCAP=$D/netdump-$ROUND.pcap
TMP=$D/tmp

# The MAC must match the HFI entry of the injected NBFT table
MAC=52:54:00:12:34:56

mkdir -p "$TMP"
for f in "$TMP/EFI/BOOT/BOOTX64.EFI" "$D/nbft-qemu.bin" "$D/nvme-boot.img"; do
        [ -e "$f" ] || { echo "ERROR: $f missing (see header)" >&2; exit 1; }
done
if [ ! -e "$D/OVMF_VARS.fd" ]; then
        cp /usr/share/OVMF/OVMF_VARS_4M.fd "$D/OVMF_VARS.fd"
fi
export TMPDIR=$D/tmpqemu
mkdir -p "$TMPDIR"
# Serial backend: "file" records the console to $LOG (automation); set
# QEMU_SERIAL=unix to open diag/serial.sock for interactive debugging, e.g.
#   socat - UNIX-CONNECT:diag/serial.sock
SERIAL_ARGS=(-serial file:"$LOG")
if [ "$QEMU_SERIAL" = unix ]; then
	rm -f "$D/serial.sock"
	# NOTE: this QEMU (10.2) rejects -serial unix:..., use -chardev
	SERIAL_ARGS=(-chardev socket,id=charserial0,path="$D/serial.sock",server=on,wait=off -serial chardev:charserial0)
fi
cd "$D"
exec timeout 300 qemu-system-x86_64 \
  -machine q35,accel=kvm -cpu host -m 1024 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file="$D/OVMF_VARS.fd" \
  -drive file=fat:rw:"$TMP",format=raw,media=disk,cache=unsafe \
  -acpitable file="$D/nbft-qemu.bin" \
  -netdev user,id=net0 \
  -device e1000,netdev=net0,mac="$MAC" \
  -object filter-dump,id=f1,netdev=net0,file="$PCAP" \
  "${SERIAL_ARGS[@]}" \
  -display none
