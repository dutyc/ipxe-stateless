#!/usr/bin/env python3
"""Parse netdump-auth.pcap: NVMe/TCP PDU summary per connection attempt."""
import os, struct, sys

def parse_pcap(path):
    data = open(path, 'rb').read()
    off = 24  # global header
    pkts = []
    while off + 16 <= len(data):
        ts_s, ts_us, incl, orig = struct.unpack('<IIII', data[off:off+16])
        off += 16
        pkt = data[off:off+incl]
        off += incl
        pkts.append((ts_s, ts_us, pkt))
    return pkts

def tcp_payload(pkt):
    # Ethernet (14) + IPv4 (20) + TCP
    if len(pkt) < 14 + 20 + 20:
        return None, None, None, None
    eth_type = struct.unpack('>H', pkt[12:14])[0]
    if eth_type != 0x0800:
        return None, None, None, None
    ip = pkt[14:]
    proto = ip[9]
    if proto != 6:
        return None, None, None, None
    ihl = (ip[0] & 0xf) * 4
    tcp = ip[ihl:]
    sport, dport = struct.unpack('>HH', tcp[0:4])
    seq = struct.unpack('>I', tcp[4:8])[0]
    flags = tcp[13]
    data_off = (tcp[12] >> 4) * 4
    payload = tcp[data_off:]
    return sport, dport, flags, payload

def pdu_summary(payload, nvmet_dir):
    """Parse all PDUs in a TCP segment. Returns list of summaries."""
    out = []
    off = 0
    while len(payload) - off >= 8:
        ptype = payload[off]
        hlen = struct.unpack('<H', payload[off+2:off+4])[0]
        plen = struct.unpack('<I', payload[off+4:off+8])[0]
        if plen < 8 or plen > len(payload) - off:
            break  # partial PDU at end of segment
        p = payload[off:off+plen]
        if ptype == 0x05:  # RSP
            if len(p) >= 24:
                cqe = p[8:24]
                cid = struct.unpack('<H', cqe[12:14])[0]
                status = struct.unpack('<H', cqe[14:16])[0]
                res = struct.unpack('<I', cqe[0:4])[0]
                out.append(f"RSP  cid={cid} status={status:#06x} result={res:#010x}")
        elif ptype == 0x04:  # COMMAND
            if len(p) >= 8 + 8:
                cid = struct.unpack('<H', p[8+2:8+4])[0]
                if p[8] == 0x7f:  # Fabrics command: fctype at dword1
                    fctype = p[8+4]
                    names = {0x00: 'PropSet', 0x01: 'Connect', 0x04: 'PropGet',
                             0x05: 'AuthSend', 0x06: 'AuthReceive'}
                    name = names.get(fctype, f'fctype={fctype:#x}')
                else:  # Regular command: label by opcode
                    opcodes = {0x01: 'Write', 0x02: 'Read', 0x06: 'Identify'}
                    name = opcodes.get(p[8], f'opcode 0x{p[8]:02x}')
                out.append(f"CMD  cid={cid} {name} plen={plen}")
        elif ptype == 0x07:  # C2HDATA
            if len(p) >= 24:
                cid = struct.unpack('<H', p[8:10])[0]
                doff = struct.unpack('<I', p[12:16])[0]
                dlen = struct.unpack('<I', p[16:20])[0]
                d = p[24:min(24+64, len(p))]
                out.append(f"C2H  cid={cid} off={doff} dlen={dlen} data={d[:48].hex()}")
        elif ptype == 0x00:
            out.append(f"ICREQ plen={plen}")
        elif ptype == 0x01:
            out.append(f"ICRESP plen={plen}")
        else:
            out.append(f"PDU type={ptype:#x} hlen={hlen} plen={plen}")
        off += plen
    return out

def main(path):
    pkts = parse_pcap(path)
    # Track connection phase by TCP seq/ack heuristics: just print all.
    prev_dir = None
    for ts_s, ts_us, pkt in pkts:
        sport, dport, flags, payload = tcp_payload(pkt)
        if payload is None and flags is None:
            continue
        if payload is None:
            if flags & 0x04:  # RST
                print(f"{ts_s}.{ts_us:06d} *** RST sport={sport} dport={dport}")
            continue
        if sport == 4420:
            d = "nvmet->"
        elif dport == 4420:
            d = "  ->nvm"
        else:
            continue
        # detect new connection (first payload after established)
        if payload:
            ss = pdu_summary(payload, d == "nvmet->")
            if ss:
                print(f"{ts_s}.{ts_us:06d} {d} {" | ".join(ss)}")

if __name__ == '__main__':
    default = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           '..', 'diag', 'netdump-auth.pcap')
    main(sys.argv[1] if len(sys.argv) > 1 else default)
