#!/usr/bin/env python3
"""TCP stream reassembly for netdump-auth.pcap.

Reassembles per-direction TCP streams and parses the full NVMe/TCP PDU
sequence, including PDUs spanning multiple TCP segments (e.g. C2HData).
"""
import os, struct, sys
from collections import defaultdict

def parse_pcap(path):
    data = open(path, 'rb').read()
    off = 24
    pkts = []
    while off + 16 <= len(data):
        ts_s, ts_us, incl, orig = struct.unpack('<IIII', data[off:off+16])
        off += 16
        pkts.append((ts_s + ts_us/1e6, data[off:off+incl]))
        off += incl
    return pkts

class Stream:
    """Reassemble one direction of a TCP connection."""
    def __init__(self):
        self.buf = bytearray()
        self.next_seq = None
        self.start = None
        self.pdus = []

    def add(self, seq, payload):
        if self.next_seq is None:
            self.start = seq
            self.next_seq = seq + len(payload)
            self.buf += payload
            return
        if seq == self.next_seq:
            self.buf += payload
            self.next_seq += len(payload)
        elif seq > self.next_seq:
            # gap (should not happen with slirp): keep for diagnostics
            self.buf += b'\x00' * (seq - self.next_seq)
            self.buf += payload
            self.next_seq = seq + len(payload)

    def pdu_hex(self, start, n):
        return bytes(self.buf[start:start+n]).hex()

def parse_stream_pdus(st, name):
    """Parse as many complete PDUs as possible from stream buffer."""
    out = []
    off = 0
    while len(st.buf) - off >= 8:
        ptype = st.buf[off]
        hlen = struct.unpack('<H', st.buf[off+2:off+4])[0]
        plen = struct.unpack('<I', st.buf[off+4:off+8])[0]
        if plen < 8 or off + plen > len(st.buf):
            break
        p = bytes(st.buf[off:off+plen])
        if ptype == 0x05:  # RSP
            cqe = p[8:24]
            cid = struct.unpack('<H', cqe[12:14])[0]
            status = struct.unpack('<H', cqe[14:16])[0]
            res = struct.unpack('<I', cqe[0:4])[0]
            out.append(f"RSP  cid={cid} status={status:#06x} result={res:#010x}")
        elif ptype == 0x04:  # COMMAND
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
            cid = struct.unpack('<H', p[8:10])[0]
            doff = struct.unpack('<I', p[12:16])[0]
            dlen = struct.unpack('<I', p[16:20])[0]
            d = p[24:min(24+80, len(p))]
            out.append(f"C2H  cid={cid} off={doff} dlen={dlen} data={d.hex()}")
        elif ptype == 0x00:
            out.append(f"ICREQ plen={plen}")
        elif ptype == 0x01:
            out.append(f"ICRESP plen={plen}")
        elif ptype == 0x08:  # C2HTERM / H2CTERM
            out.append(f"TERM plen={plen}")
        else:
            out.append(f"PDU type={ptype:#x} hlen={hlen} plen={plen}")
        off += plen
    del st.buf[:off]
    return out

def main(path):
    pkts = parse_pcap(path)
    # streams keyed by (src_ip, sport, dst_ip, dport)
    streams = {}
    conns = defaultdict(list)  # (src_ip,sport,dst_ip,dport) -> [flags...]
    for ts, pkt in pkts:
        if len(pkt) < 14 + 20 + 20:
            continue
        eth_type = struct.unpack('>H', pkt[12:14])[0]
        if eth_type != 0x0800:
            continue
        ip = pkt[14:]
        if ip[9] != 6:
            continue
        ihl = (ip[0] & 0xf) * 4
        ip_len = struct.unpack('>H', ip[2:4])[0]
        tcp = ip[ihl:]
        sport, dport = struct.unpack('>HH', tcp[0:4])
        seq = struct.unpack('>I', tcp[4:8])[0]
        flags = tcp[13]
        data_off = (tcp[12] >> 4) * 4
        # 用 IP 总长计算 payload 边界，排除以太网最小帧 60 字节的 padding
        plen = ip_len - ihl - data_off
        payload = tcp[data_off:data_off + plen]
        src = '.'.join(str(b) for b in ip[12:16])
        dst = '.'.join(str(b) for b in ip[16:20])
        if dport != 4420 and sport != 4420:
            continue
        key = (src, sport, dst, dport)
        if payload:
            s = streams.get(key)
            if s is None:
                s = Stream()
                streams[key] = s
            s.add(seq, payload)
        if flags & 0x04:
            print(f"{ts:.6f} *** RST {src}:{sport} -> {dst}:{dport}")
        if flags & 0x01:
            print(f"{ts:.6f} *** FIN {src}:{sport} -> {dst}:{dport}")

    # Print stream PDU sequences in arrival order of first packet
    order = []
    seen = set()
    for ts, pkt in pkts:
        # same filtering to find keys in order
        if len(pkt) < 14 + 20 + 20:
            continue
        eth_type = struct.unpack('>H', pkt[12:14])[0]
        if eth_type != 0x0800:
            continue
        ip = pkt[14:]
        if ip[9] != 6:
            continue
        ihl = (ip[0] & 0xf) * 4
        ip_len = struct.unpack('>H', ip[2:4])[0]
        tcp = ip[ihl:]
        sport, dport = struct.unpack('>HH', tcp[0:4])
        data_off = (tcp[12] >> 4) * 4
        plen = ip_len - ihl - data_off
        payload = tcp[data_off:data_off + plen]
        if dport != 4420 and sport != 4420:
            continue
        src = '.'.join(str(b) for b in ip[12:16])
        dst = '.'.join(str(b) for b in ip[16:20])
        key = (src, sport, dst, dport)
        if key not in seen and payload:
            seen.add(key)
            order.append(key)

    # parse all streams, interleave by connection
    for key in order:
        src, sport, dst, dport = key
        s = streams[key]
        pdus = parse_stream_pdus(s, f"{src}:{sport}->{dst}:{dport}")
        if pdus:
            print(f"== stream {src}:{sport} -> {dst}:{dport}")
            for p in pdus:
                print(f"   {p}")

if __name__ == '__main__':
    default = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           '..', 'diag', 'netdump-auth.pcap')
    main(sys.argv[1] if len(sys.argv) > 1 else default)
