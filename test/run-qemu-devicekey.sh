#!/usr/bin/env bash
# Run one QEMU round of the device key command self-test:
#   round 1: clean NVRAM -> keygen generates a key, pubkey and sign work
#   round 2: kept NVRAM  -> keygen refuses to overwrite, sign still works
#
# The two rounds MUST share diag/OVMF_VARS.fd.  Delete that file
# before round 1 to start from a clean NVRAM.
#
# Usage: bash test/run-qemu-devicekey.sh [1|2]
#
# Prerequisites:
#   build the firmware with test/devicekey-test.ipxe embedded and deploy it:
#     make -C .cache/ipxe-upstream/src bin-x86_64-efi/ipxe.efi \
#       EMBED=test/devicekey-test.ipxe
#     cp .cache/ipxe-upstream/src/bin-x86_64-efi/ipxe.efi \
#        diag/tmp/EFI/BOOT/BOOTX64.EFI
#
# Output: diag/qemu-devicekey-<round>.log
# Run outside sandbox (requires /dev/kvm).
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
D=$ROOT/diag
ROUND=${1:-1}
LOG=$D/qemu-devicekey-$ROUND.log
TMP=$D/tmp

[ "$ROUND" = 1 ] || [ "$ROUND" = 2 ] || {
        echo "usage: $0 [1|2]" >&2
        exit 1
}

mkdir -p "$TMP"
[ -e "$TMP/EFI/BOOT/BOOTX64.EFI" ] || {
        echo "ERROR: $TMP/EFI/BOOT/BOOTX64.EFI missing (see header)" >&2
        exit 1
}

# Round 1 starts from a clean NVRAM
[ "$ROUND" = 1 ] && rm -f "$D/OVMF_VARS.fd"
if [ ! -e "$D/OVMF_VARS.fd" ]; then
        cp /usr/share/OVMF/OVMF_VARS_4M.fd "$D/OVMF_VARS.fd"
fi

export TMPDIR=$D/tmpqemu
mkdir -p "$TMPDIR"
cd "$D"
timeout 90 qemu-system-x86_64 \
  -machine q35,accel=kvm -cpu host -m 1024 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file="$D/OVMF_VARS.fd" \
  -drive file=fat:rw:"$TMP",format=raw,media=disk,cache=unsafe \
  -netdev user,id=net0 \
  -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
  -serial file:"$LOG" \
  -display none
