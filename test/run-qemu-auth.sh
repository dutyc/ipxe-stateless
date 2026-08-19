#!/usr/bin/env bash
# Run one QEMU round of NVMe/TCP validation against the local nvmet target.
#
# Usage: bash test/run-qemu-auth.sh [round]     (default round: "auth")
#
# - serial log and pcap are written to diag/ as qemu-<round>.log and
#   netdump-<round>.log (diag/ is gitignored: runtime artifacts stay local)
# - the FAT drive diag/tmp/ must contain the test firmware at
#   EFI/BOOT/BOOTX64.EFI, e.g. built with
#   EMBED=test/nvmeof-auth-test.ipxe DEBUG=nvmetcp,nvmetcp_auth:3
# - note: QEMU+OVMF provides no SMBIOS UUID to iPXE, so the host NQN
#   falls back to "nqn.2014-08.org.ipxe:ipxe" (see test/nvmet-setup.sh)
# - run outside sandbox (requires /dev/kvm)
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
D=$ROOT/diag
ROUND=${1:-auth}
LOG=$D/qemu-$ROUND.log
PCAP=$D/netdump-$ROUND.pcap
TMP=$D/tmp

mkdir -p "$TMP"
if [ ! -e "$TMP/EFI/BOOT/BOOTX64.EFI" ]; then
        echo "ERROR: $TMP/EFI/BOOT/BOOTX64.EFI missing" >&2
        echo "       build the test firmware and copy it into the FAT drive:" >&2
        echo "         cp .cache/ipxe-upstream/src/bin-x86_64-efi/ipxe.efi $TMP/EFI/BOOT/BOOTX64.EFI" >&2
        exit 1
fi
if [ ! -e "$D/OVMF_VARS.fd" ]; then
        cp /usr/share/OVMF/OVMF_VARS_4M.fd "$D/OVMF_VARS.fd"
fi
export TMPDIR=$D/tmpqemu
mkdir -p "$TMPDIR"
cd "$D"
exec timeout 150 qemu-system-x86_64 \
  -machine q35,accel=kvm -cpu host -m 1024 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file="$D/OVMF_VARS.fd" \
  -drive file=fat:rw:"$TMP",format=raw,media=disk,cache=unsafe \
  -netdev user,id=net0 \
  -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
  -object filter-dump,id=f1,netdev=net0,file="$PCAP" \
  -serial file:"$LOG" \
  -display none
