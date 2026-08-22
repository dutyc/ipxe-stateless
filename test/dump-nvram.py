#!/usr/bin/env python3
# Dump the IpXeStatelessNvo EFI variable payload from an OVMF vars file
# and search it for a given certificate SHA-256 fingerprint.
#
# Usage: dump-nvram.py <OVMF_VARS.fd> [<fingerprint-hex>]
import struct
import sys

path = sys.argv[1]
want = sys.argv[2].replace(":", "") if len(sys.argv) > 2 else None
data = open(path, "rb").read()

name = "IpXeStatelessNvo".encode("utf-16le")
# Use the latest copy: OVMF keeps stale copies of rewritten variables
pos = data.rfind(name)
if pos < 0:
    print("variable not found")
    sys.exit(1)

# EFI_VARIABLE_HEADER precedes the name: StartId(2) State(1) Reserved(1)
# Attributes(4) NameSize(4) DataSize(4) VendorGuid(16)
hdr = pos - 32
start_id, state, _res, attrs, name_size, data_size = struct.unpack_from(
    "<HBBI II", data, hdr)
print(f"header at {hdr:#x}: start_id={start_id:#06x} state={state:#04x} "
      f"attrs={attrs:#x} name_size={name_size} data_size={data_size}")

payload = pos + name_size
payload = (payload + 3) & ~3  # data is 4-byte aligned after the name
blob = data[payload:payload + data_size]
print(f"payload at {payload:#x}, {data_size} bytes:")
print(blob.hex())

if want:
    if bytes.fromhex(want) in blob:
        print(f"MATCH: fingerprint {want} found in variable")
    else:
        print(f"MISMATCH: fingerprint {want} not found")
        sys.exit(1)
