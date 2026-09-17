#!/usr/bin/env python3
"""Read a tcpdump capture of the E7 uploads and say what TCP did.

No dependencies: pcap is a header and a list of records, and the only layers
that matter here are Ethernet, IPv4 and TCP. tshark is not installed on this
machine and scapy is not in the server's venv, so parsing it by hand is the
short path rather than the clever one.

**The payload length comes from the IP header, not from what was captured.**
The capture is taken with -s 128, which keeps the headers and throws the data
away -- so counting the bytes that are actually in the file would report a
tenth of the upload.

The question the capture exists to settle is which end lost something, and a
server-side capture can tell:

  hole    a segment arrives past the end of what has been received, so the one
          before it never got here. That is a loss on the way up, and the
          retransmission that fills the hole is the device doing the right
          thing.
  repeat  a segment arrives whose bytes are already here. Nothing was lost on
          the way up; the device retransmitted anyway, which means it never
          saw the ACK -- the loss was on the way back -- or it gave up waiting.

Two modes:

    tools/e7_pcap.py /tmp/e7.pcap          every connection, one line each
    tools/e7_pcap.py /tmp/e7.pcap 32       every packet of connection 32
"""

import struct
import sys
from collections import defaultdict

GAP_MS = 150.0


def packets(path):
    with open(path, "rb") as fh:
        magic = fh.read(4)
        if magic == b"\xd4\xc3\xb2\xa1":
            endian, scale = "<", 1e-6
        elif magic == b"\xa1\xb2\xc3\xd4":
            endian, scale = ">", 1e-6
        elif magic == b"\x4d\x3c\xb2\xa1":
            endian, scale = "<", 1e-9
        elif magic == b"\xa1\xb2\x3c\x4d":
            endian, scale = ">", 1e-9
        else:
            sys.exit(f"{path}: not a pcap file (magic {magic!r})")

        _, _, _, _, _, link = struct.unpack(endian + "HHiIII", fh.read(20))
        while True:
            hdr = fh.read(16)
            if len(hdr) < 16:
                return
            sec, frac, caplen, _ = struct.unpack(endian + "IIII", hdr)
            data = fh.read(caplen)
            if len(data) < caplen:
                return
            yield sec + frac * scale, link, data


def parse(link, data):
    if link == 1:  # EN10MB
        if len(data) < 14 or struct.unpack("!H", data[12:14])[0] != 0x0800:
            return None
        ip = data[14:]
    elif link == 0:  # NULL/loopback
        ip = data[4:]
    else:
        return None

    if len(ip) < 20 or (ip[0] >> 4) != 4:
        return None
    ihl = (ip[0] & 0x0F) * 4
    total = struct.unpack("!H", ip[2:4])[0]
    if ip[9] != 6:
        return None
    src = ".".join(str(b) for b in ip[12:16])
    dst = ".".join(str(b) for b in ip[16:20])

    tcp = ip[ihl:]
    if len(tcp) < 20:
        return None
    sport, dport, seq, ack = struct.unpack("!HHII", tcp[:12])
    off = (tcp[12] >> 4) * 4
    flags = tcp[13]
    window = struct.unpack("!H", tcp[14:16])[0]
    # The declared length, not the captured one: -s 128 keeps the headers only.
    payload = max(total - ihl - off, 0)
    return src, sport, dst, dport, seq, ack, flags, window, payload


def new_flow():
    return {
        "syn": None, "synack": None, "first": None, "last": None, "isn": None,
        "bytes": 0, "segments": 0, "high": 0, "holes": [], "repeats": [],
        "gaps": [], "rst": False, "zerowin": 0, "dupack": 0, "prev": None,
        "acks": defaultdict(int), "synretx": 0,
    }


def main(path, server_port=8000):
    flows, order = {}, []

    for ts, link, data in packets(path):
        parsed = parse(link, data)
        if parsed is None:
            continue
        src, sport, dst, dport, seq, ack, flags, window, payload = parsed

        if dport == server_port:
            key, inbound = (src, sport), True
        elif sport == server_port:
            key, inbound = (dst, dport), False
        else:
            continue

        if key not in flows:
            flows[key] = new_flow()
            order.append(key)
        flow = flows[key]

        syn, ack_f, fin, rst = flags & 0x02, flags & 0x10, flags & 0x01, flags & 0x04

        if inbound:
            if syn and not ack_f:
                if flow["syn"] is None:
                    flow["syn"], flow["isn"] = ts, (seq + 1) & 0xFFFFFFFF
                else:
                    flow["synretx"] += 1
            if rst:
                flow["rst"] = True
            if payload and flow["isn"] is not None:
                rel = (seq - flow["isn"]) & 0xFFFFFFFF
                if flow["first"] is None:
                    flow["first"] = ts
                since = (ts - flow["syn"]) * 1000.0

                if rel > flow["high"]:
                    # Past the end of what is here: the segment before it is missing.
                    flow["holes"].append((since, flow["high"], rel - flow["high"]))
                    flow["high"] = rel + payload
                    flow["bytes"] += payload
                elif rel + payload <= flow["high"]:
                    # Every byte of it is already here.
                    flow["repeats"].append((since, rel, payload))
                else:
                    flow["bytes"] += rel + payload - flow["high"]
                    flow["high"] = rel + payload

                flow["segments"] += 1
                if flow["prev"] is not None:
                    gap = (ts - flow["prev"]) * 1000.0
                    if gap >= GAP_MS:
                        flow["gaps"].append((gap, flow["high"], since))
                flow["prev"] = ts
                flow["last"] = ts
        else:
            if syn and ack_f and flow["synack"] is None:
                flow["synack"] = ts
            if window == 0:
                flow["zerowin"] += 1
            if ack_f and payload == 0 and not syn and not fin:
                flow["acks"][ack] += 1
                if flow["acks"][ack] > 1:
                    flow["dupack"] += 1

    print(f"{len(order)} connections to port {server_port}\n")
    header = (f"{'#':>3} {'client':>8} {'SYN->ACK':>9} {'KB in':>7} {'segs':>5} "
              f"{'body ms':>8} {'KB/s':>6} {'holes':>6} {'repeat':>7} {'gaps':>5} "
              f"{'0win':>5} {'dupack':>7}  note")
    print(header)
    print("-" * len(header))

    totals = {"holes": 0, "repeats": 0, "conns": 0, "stalled": 0}
    for i, key in enumerate(order, 1):
        f = flows[key]
        hand = f"{(f['synack'] - f['syn']) * 1000:.1f}" if f["syn"] and f["synack"] else "--"
        if f["first"] and f["last"] and f["last"] > f["first"]:
            body = (f["last"] - f["first"]) * 1000.0
            rate, body_s = f"{f['bytes'] / 1024 / (body / 1000):.0f}", f"{body:.0f}"
        else:
            body_s, rate = "--", "--"
        note = []
        if f["syn"] and not f["synack"]:
            note.append("SYN never answered")
        if f["synretx"]:
            note.append(f"{f['synretx']} SYN retransmits")
        if f["rst"]:
            note.append("RST")
        totals["conns"] += 1
        totals["holes"] += len(f["holes"])
        totals["repeats"] += len(f["repeats"])
        if any(g[0] >= 400 for g in f["gaps"]):
            totals["stalled"] += 1
        print(f"{i:>3} {key[1]:>8} {hand:>9} {f['bytes']/1024:>7.0f} {f['segments']:>5} "
              f"{body_s:>8} {rate:>6} {len(f['holes']):>6} {len(f['repeats']):>7} "
              f"{len(f['gaps']):>5} {f['zerowin']:>5} {f['dupack']:>7}  {', '.join(note)}")

    print(f"\n{totals['stalled']}/{totals['conns']} connections stopped for 400 ms or more."
          f" {totals['holes']} holes (lost on the way up),"
          f" {totals['repeats']} repeats (arrived twice).\n")

    for i, key in enumerate(order, 1):
        f = flows[key]
        events = [(g[2], f"gap {g[0]:8.1f} ms, {g[1] // 1024:>3} KB in") for g in f["gaps"]
                  if g[0] >= 400]
        events += [(h[0], f"hole at {h[1] // 1024:>3} KB: {h[2]} bytes missing") for h in f["holes"]]
        events += [(r[0], f"repeat at {r[1] // 1024:>3} KB ({r[2]} B) -- already had it")
                   for r in f["repeats"]]
        if not events:
            continue
        print(f"connection {i} (port {key[1]}):")
        for when, what in sorted(events):
            print(f"   {when:8.1f} ms in   {what}")
        print()


def detail(path, server_port, want):
    """Every packet of one connection, both directions.

    The summary says the backend already had the bytes the device sent again.
    That leaves two ways for the device to have been waiting: the backend's ACK
    went out and was lost on the way down, or it never went out. Only a capture
    showing both directions can say which, and this one can -- it is taken on
    the backend, so an ACK it emitted is in the file whether or not it arrived.
    """
    conns, order = {}, []
    for ts, link, data in packets(path):
        p = parse(link, data)
        if p is None:
            continue
        src, sport, dst, dport, seq, ack, flags, window, payload = p
        key = (src, sport) if dport == server_port else (
            (dst, dport) if sport == server_port else None)
        if key is None:
            continue
        if key not in conns:
            conns[key] = []
            order.append(key)
        conns[key].append((ts, dport == server_port, seq, ack, flags, window, payload))

    key = order[want - 1]
    pkts = conns[key]
    t0 = pkts[0][0]
    isn_up = isn_dn = None
    print(f"connection {want} (port {key[1]}), {len(pkts)} packets\n")
    print(f"{'ms':>9}  {'dir':^4} {'flags':<6} {'seq':>9} {'ack':>9} {'len':>5} {'win':>7}  note")

    prev_up = None
    for ts, up, seq, ack, flags, window, payload in pkts:
        if up and isn_up is None:
            isn_up = seq
        if not up and isn_dn is None:
            isn_dn = seq
        rel_seq = (seq - (isn_up if up else isn_dn)) & 0xFFFFFFFF
        rel_ack = (ack - ((isn_dn if up else isn_up) or 0)) & 0xFFFFFFFF
        names = "".join(n for bit, n in ((0x02, "S"), (0x10, "A"), (0x01, "F"),
                                         (0x04, "R"), (0x08, "P")) if flags & bit)
        note = ""
        if up and payload:
            if prev_up is not None and (ts - prev_up) * 1000 >= 400:
                note = f"<- {(ts - prev_up) * 1000:.0f} ms of silence from the device"
            prev_up = ts
        print(f"{(ts - t0) * 1000:9.1f}  {'up' if up else 'down':^4} {names:<6} {rel_seq:>9} "
              f"{rel_ack if flags & 0x10 else 0:>9} {payload:>5} {window:>7}  {note}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(f"usage: {sys.argv[0]} <capture.pcap> [connection number] [server port]")
    capture = sys.argv[1]
    which = int(sys.argv[2]) if len(sys.argv) > 2 else None
    port = int(sys.argv[3]) if len(sys.argv) > 3 else 8000
    if which is None:
        main(capture, port)
    else:
        detail(capture, port, which)
