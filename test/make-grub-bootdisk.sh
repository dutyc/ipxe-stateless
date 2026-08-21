#!/bin/bash
# Build a GRUB-bootable NVMe/TCP backing disk for QEMU validation.
# No root required: GPT via parted, ESP via mkfs.vfat + mtools,
# GRUB EFI binary via grub-mkimage.
# Usage: bash test/make-grub-bootdisk.sh
set -e

# Runtime artifacts (the disk image) are written to diag/ (gitignored)
ROOT=$(cd "$(dirname "$0")/.." && pwd)
D=$ROOT/diag
IMG=$D/nvme-boot.img
ESP=$D/.esp.img
STAGE=$D/.esp-stage

SIZE_M=512
# ESP must be >= 65525 clusters for FAT32 (4K clusters => ~256 MiB),
# otherwise EDK2 FatDxe may reject the BPB.
ESP_M=300
# ESP starts at 4K-sector LBA 2048 (= 8 MiB) in the 4K-sector GPT
ESP_START_M=8

echo "==> Tools"
for t in python3 mkfs.vfat mcopy grub-mkimage; do
	command -v "$t" >/dev/null || { echo "missing: $t"; exit 1; }
done

echo "==> Blank disk image (${SIZE_M}M)"
truncate -s "${SIZE_M}M" "$IMG"

echo "==> GPT: 1 ESP partition (${ESP_M}M at ${ESP_START_M}M), 4K sectors"
# nvmet file backend exposes 4096-byte logical blocks; the EFI
# PartitionDxe reads LBA1 at BlockSize granularity, so the disk must
# be formatted with 4K sectors (GPT header at file offset 4096).
python3 - "$IMG" <<'PYEOF'
import struct, sys, zlib

path = sys.argv[1]
sector = 4096
sectors = 512 * 1024 * 1024 // sector      # 131072 x 4K sectors
first_usable = 34                           # 128 entries x 128 B = 4 sectors
last_usable = sectors - 34 - 1
esp_start = 2048                            # 8 MiB
esp_sectors = 300 * 1024 * 1024 // sector  # 76800 x 4K sectors (> 65525 FAT32 min)
esp_end = esp_start + esp_sectors - 1

def guid_bytes(u):
    # GPT GUIDs are stored mixed-endian: the first three components are
    # little-endian (EFI spec).  Writing a UUID string as plain hex would
    # read back with those fields byte-swapped.
    b = bytes.fromhex(u)
    return b[3::-1] + b[5:3:-1] + b[7:5:-1] + b[8:]

# Protective MBR (LBA0)
mbr = bytearray(sector)
mbr[0x1BE + 4] = 0xEE                      # partition type 0xEE
mbr[0x1BE + 8:0x1BE + 12] = struct.pack('<I', 1)
mbr[0x1BE + 12:0x1BE + 16] = struct.pack('<I', sectors - 1)
mbr[0x1FE:0x200] = b'\x55\xaa'

# GPT header (LBA1)
header = bytearray(92)
header[0:8] = b'EFI PART'
header[8:12] = struct.pack('<I', 0x00010000)     # revision 1.0
header[12:16] = struct.pack('<I', 92)            # header size
header[24:32] = struct.pack('<Q', 1)             # current LBA
header[32:40] = struct.pack('<Q', sectors - 1)   # backup LBA
header[40:48] = struct.pack('<Q', first_usable)
header[48:56] = struct.pack('<Q', last_usable)
header[56:72] = guid_bytes('4dc45c6de8a6bf47b0b0009e0c71d2c1')  # disk GUID
header[72:80] = struct.pack('<Q', 2)             # partition entry LBA
header[80:84] = struct.pack('<I', 128)           # entries
header[84:88] = struct.pack('<I', 128)           # entry size

# Partition table (LBA2-5): one ESP entry
part = bytearray(128 * 128)
part[0:16] = guid_bytes('c12a7328f81f11d2ba4b00a0c93ec93b')  # ESP type GUID
part[16:32] = guid_bytes('00a0c9e1933e4b88b8d1e2a90d1c2e3f')  # unique GUID
part[32:40] = struct.pack('<Q', esp_start)
part[40:48] = struct.pack('<Q', esp_end)
part[48:56] = struct.pack('<Q', 0)               # attributes
name = 'EFI System Partition'.encode('utf-16le')
part[56:56 + len(name)] = name
header[88:92] = struct.pack('<I', zlib.crc32(part))        # table CRC
header[16:20] = struct.pack('<I', zlib.crc32(header))      # header CRC

with open(path, 'r+b') as f:
    f.seek(0)
    f.write(mbr)
    f.seek(sector)
    f.write(header)
    f.seek(2 * sector)
    f.write(part)
    # Backup GPT: partition table at canonical location last_usable+1
    # (EDK2 PartitionDxe cross-checks it there; sectors-5 would leave
    # zeroes at that LBA and fail the CRC check).
    f.seek((last_usable + 1) * sector)
    f.write(part)
    f.seek((sectors - 1) * sector)
    header[24:32] = struct.pack('<Q', sectors - 1)  # current LBA = backup
    header[32:40] = struct.pack('<Q', 1)            # backup LBA = LBA1
    header[72:80] = struct.pack('<Q', last_usable + 1)  # backup table LBA
    header[16:20] = struct.pack('<I', 0)
    header[16:20] = struct.pack('<I', zlib.crc32(header))
    f.write(header)
print('GPT (4K sectors) written: ESP LBA %d-%d' % (esp_start, esp_end))
PYEOF

echo "==> ESP filesystem image (4K sector FAT32)"
rm -f "$ESP"
truncate -s "${ESP_M}M" "$ESP"
mkfs.vfat -F 32 -S 4096 -n IPXENVME "$ESP"

echo "==> GRUB EFI binary + config"
rm -rf "$STAGE"
mkdir -p "$STAGE/EFI/BOOT"
# EFI disk driver (efidisk) is built into the GRUB 2.14 core
# image, so no disk module is required here.  echo is a separate
# module in GRUB 2.14 (grub.cfg menuentry uses it).
grub-mkimage -O x86_64-efi -o "$STAGE/EFI/BOOT/BOOTX64.EFI" -p /EFI/BOOT \
	fat part_gpt normal configfile serial ls sleep test echo

cat > "$STAGE/EFI/BOOT/grub.cfg" <<'EOF'
serial --unit=0 --speed=115200
terminal_input serial
terminal_output serial
set timeout=5
set default=0
menuentry "NVMe/TCP SAN boot OK" {
	echo "== GRUB BOOTED FROM NVME/TCP SAN DISK =="
	ls
	sleep --interruptible 60
}
EOF

echo "==> Populate ESP"
mcopy -i "$ESP" -s "$STAGE"/* ::/

echo "==> Write ESP into disk image (offset ${ESP_START_M}M)"
dd if="$ESP" of="$IMG" bs=1M seek="$ESP_START_M" conv=notrunc status=none

echo "==> Verify"
ls -la "$IMG"
mdir -i "$ESP" ::/EFI/BOOT/
rm -rf "$STAGE" "$ESP"
echo "==> DONE: $IMG"
