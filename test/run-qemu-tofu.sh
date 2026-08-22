#!/usr/bin/env bash
# Run one QEMU round of the TOFU verification:
#   round 1: empty NVRAM + server cert A  -> accept handshake, store fingerprint
#   round 2: kept NVRAM + server cert A   -> fingerprint matches, download OK
#   round 3: kept NVRAM + server cert B   -> fingerprint mismatch, reject
#
# The three rounds MUST share diag/OVMF_VARS.fd.  Delete that file
# before round 1 to start from a clean NVRAM.
#
# Usage: bash test/run-qemu-tofu.sh [1|2|3]
#
# Prerequisites:
#   build the firmware with test/tofu-test.ipxe embedded and deploy it:
#     make -C .cache/ipxe-upstream/src bin-x86_64-efi/ipxe.efi \
#       EMBED=test/tofu-test.ipxe
#     cp .cache/ipxe-upstream/src/bin-x86_64-efi/ipxe.efi \
#        diag/tmp/EFI/BOOT/BOOTX64.EFI
#
# Output: diag/qemu-tofu-<round>.log
# Run outside sandbox (requires /dev/kvm).
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
D=$ROOT/diag
ROUND=${1:-1}
LOG=$D/qemu-tofu-$ROUND.log
TMP=$D/tmp
TLS=$ROOT/test/tls
PORT=8443

[ "$ROUND" = 1 ] || [ "$ROUND" = 2 ] || [ "$ROUND" = 3 ] || {
        echo "usage: $0 [1|2|3]" >&2
        exit 1
}

mkdir -p "$TMP" "$TLS" "$TMP/tlsroot"
[ -e "$TMP/EFI/BOOT/BOOTX64.EFI" ] || {
        echo "ERROR: $TMP/EFI/BOOT/BOOTX64.EFI missing (see header)" >&2
        exit 1
}
[ -e "$TMP/tlsroot/test.txt" ] || echo "TOFU test payload" > "$TMP/tlsroot/test.txt"

# Round 1 starts from a clean NVRAM
[ "$ROUND" = 1 ] && rm -f "$D/OVMF_VARS.fd"
if [ ! -e "$D/OVMF_VARS.fd" ]; then
        cp /usr/share/OVMF/OVMF_VARS_4M.fd "$D/OVMF_VARS.fd"
fi

# Generate the server certificates if not yet present
if [ ! -e "$TLS/certA.pem" ]; then
        openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
                -keyout "$TLS/keyA.pem" -out "$TLS/certA.pem" \
                -subj "/CN=10.0.2.2" \
                -addext "subjectAltName=IP:10.0.2.2" >/dev/null 2>&1
fi
if [ ! -e "$TLS/certB.pem" ]; then
        openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
                -keyout "$TLS/keyB.pem" -out "$TLS/certB.pem" \
                -subj "/CN=10.0.2.2" \
                -addext "subjectAltName=IP:10.0.2.2" >/dev/null 2>&1
fi

# Start the HTTPS server (cert A for rounds 1/2, cert B for round 3)
CERT=$TLS/certA.pem
KEY=$TLS/keyA.pem
[ "$ROUND" = 3 ] && { CERT=$TLS/certB.pem; KEY=$TLS/keyB.pem; }
python3 "$ROOT/test/https-server.py" "$PORT" "$CERT" "$KEY" "$TMP/tlsroot" \
        >"$D/https-server.log" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null || true' EXIT
sleep 1

export TMPDIR=$D/tmpqemu
mkdir -p "$TMPDIR"
cd "$D"
timeout 120 qemu-system-x86_64 \
  -machine q35,accel=kvm -cpu host -m 1024 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file="$D/OVMF_VARS.fd" \
  -drive file=fat:rw:"$TMP",format=raw,media=disk,cache=unsafe \
  -netdev user,id=net0 \
  -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
  -serial file:"$LOG" \
  -display none
