#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""An RTL8152 USB Fast Ethernet adapter, emulated for QEMU's usb-redir device.

    rtl8152emu.py PORT      then: qemu ... -device qemu-xhci,id=x
                                  -chardev socket,id=ur,host=127.0.0.1,port=PORT
                                  -device usb-redir,chardev=ur,bus=x.0

It is the device end of the usbredir protocol (usbredirproto.h): the real
chip's descriptors (vendor configuration 1, CDC-ECM configuration 2), the
vendor register protocol (bRequest 5: PLA / USB spaces, byte enables) with
the behaviour the driver waits on (chip version, OOB link-list ready, the
self-clearing reset, the PHY's link after autonegotiation), and frames on
bulk endpoints 0x81 / 0x02 with Realtek's 24-byte receive header and 8-byte
transmit header. Behind it, a tiny network: a DHCP server, ARP and ping
for 10.0.2.2. Everything the guest does is logged.
"""
import socket, struct, sys, select, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 7777
VERSION_REG = 0x4c10                       # RTL8152B
MAC = bytes([0x00, 0xe0, 0x4c, 0x68, 0x01, 0x52])
GW_MAC = bytes([0x52, 0x55, 0x0a, 0x00, 0x02, 0x02])
GW_IP, CLIENT_IP, MASK = bytes([10, 0, 2, 2]), bytes([10, 0, 2, 15]), bytes([255, 255, 255, 0])

def log(*a):
    print("[%.3f]" % (time.time() - T0), *a, flush=True)
T0 = time.time()

# ── descriptors ──
def dev_desc():
    return struct.pack("<BBHBBBBHHHBBBB", 18, 1, 0x0210, 0, 0, 0, 64, 0x0bda, 0x8152, 0x2000, 1, 2, 3, 2)

def cfg1():
    ifd = struct.pack("<BBBBBBBBB", 9, 4, 0, 0, 3, 0xff, 0xff, 0, 0)
    eps = (struct.pack("<BBBBHB", 7, 5, 0x81, 2, 512, 0) + struct.pack("<BBBBHB", 7, 5, 0x02, 2, 512, 0) +
           struct.pack("<BBBBHB", 7, 5, 0x83, 3, 2, 8))
    body = ifd + eps
    return struct.pack("<BBHBBBBB", 9, 2, 9 + len(body), 1, 1, 0, 0xa0, 50) + body

def cfg2():                                 # CDC-ECM, as the real chip offers too
    i0 = struct.pack("<BBBBBBBBB", 9, 4, 0, 0, 1, 2, 6, 0, 5)
    f = bytes([5, 0x24, 0, 0x10, 0x01]) + bytes([5, 0x24, 6, 0, 1]) + bytes([13, 0x24, 0x0f, 4, 0, 0, 0, 0, 0xea, 5, 0, 0, 0])
    e0 = struct.pack("<BBBBHB", 7, 5, 0x83, 3, 16, 8)
    i1a = struct.pack("<BBBBBBBBB", 9, 4, 1, 0, 0, 0x0a, 0, 0, 0)
    i1b = struct.pack("<BBBBBBBBB", 9, 4, 1, 1, 2, 0x0a, 0, 0, 0)
    e1 = struct.pack("<BBBBHB", 7, 5, 0x81, 2, 512, 0) + struct.pack("<BBBBHB", 7, 5, 0x02, 2, 512, 0)
    body = i0 + f + e0 + i1a + i1b + e1
    return struct.pack("<BBHBBBBB", 9, 2, 9 + len(body), 2, 2, 0, 0xa0, 50) + body

STRINGS = {1: "Realtek", 2: "USB 10/100 LAN", 3: "000001", 4: "000001", 5: "00E04C680152"}
def string_desc(i):
    if i == 0: return bytes([4, 3, 0x09, 0x04])
    s = STRINGS.get(i, "?").encode("utf-16-le")
    return bytes([2 + len(s), 3]) + s

# ── the chip's registers ──
class Chip:
    def __init__(self):
        self.mem = {0x100: bytearray(0x10000), 0: bytearray(0x10000)}   # PLA, USB
        pla = self.mem[0x100]
        pla[0xc000:0xc006] = MAC                  # PLA_IDR
        pla[0xd000:0xd006] = MAC                  # PLA_BACKUP (where the B reads it)
        struct.pack_into("<H", pla, 0xe612, VERSION_REG)   # PLA_TCR1
        self.rx_on = False
        self.neg_at = None                         # when autonegotiation started
        self.writes = 0
    def read(self, space, addr, n):
        m = self.mem[space]
        if space == 0x100:
            m[0xe84f] |= 0x02                      # OOB_CTRL: LINK_LIST_READY
            if self.neg_at and time.time() - self.neg_at > 0.8:
                struct.pack_into("<H", m, 0xe908, 0x0002 | 0x0008 | 0x0001)   # PHYSTATUS: link, 100, full duplex
        return bytes(m[addr:addr + n])
    def write(self, space, addr, en, data):
        m = self.mem[space]
        self.writes += 1
        for i, b in enumerate(data):
            if en & (1 << i): m[addr + i] = b
        if space == 0x100:
            if m[0xe813] & 0x10:                   # CR_RST: done at once
                m[0xe813] &= ~0x10 & 0xff
                log("chip: reset")
            re = bool(m[0xe813] & 0x08)
            if re != self.rx_on:
                self.rx_on = re
                log("chip: receive", "on" if re else "off", "(CR=%02x)" % m[0xe813])
            # the PHY window: OCP_GPHY_BASE (0xe86c) + 0xb000..0xbfff
            if 0xb000 <= addr < 0xc000:
                base = struct.unpack_from("<H", m, 0xe86c)[0]
                for i in range(0, 4, 2):
                    if en & (3 << i) == (3 << i):
                        ocp = base | ((addr + i) & 0x0fff)
                        val = struct.unpack_from("<H", bytes(data), i)[0]
                        if ocp == 0xa400 and val & 0x0200:   # BMCR: restart autonegotiation
                            log("phy: autonegotiation started")
                            self.neg_at = time.time()

# ── a tiny network behind the cable ──
def csum(b):
    if len(b) % 2: b += b"\0"
    s = sum(struct.unpack("!%dH" % (len(b) // 2), b))
    while s >> 16: s = (s & 0xffff) + (s >> 16)
    return (~s) & 0xffff

def ip_packet(proto, src, dst, payload):
    h = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), 0, 0, 64, proto, 0, src, dst)
    h = h[:10] + struct.pack("!H", csum(h)) + h[12:]
    return h + payload

def udp(src, dst, sport, dport, data):
    return ip_packet(17, src, dst, struct.pack("!HHHH", sport, dport, 8 + len(data), 0) + data)

def dhcp_reply(req, kind):
    xid = req[4:8]; chaddr = req[28:44]
    bootp = struct.pack("!BBBB", 2, 1, 6, 0) + xid + b"\0" * 4 + b"\0" * 4 + CLIENT_IP + GW_IP + b"\0" * 4 + chaddr + b"\0" * 192
    opts = (b"\x63\x82\x53\x63" + bytes([53, 1, kind]) + bytes([54, 4]) + GW_IP + bytes([51, 4]) + struct.pack("!I", 86400) +
            bytes([1, 4]) + MASK + bytes([3, 4]) + GW_IP + bytes([6, 4]) + GW_IP + b"\xff")
    return bootp + opts

class Net:
    def __init__(self, emit):
        self.emit = emit
        self.stats = {"dhcp": [], "arp": 0, "ping": 0, "tx": 0}
    def frame_from_guest(self, f):
        self.stats["tx"] += 1
        if len(f) < 14: return
        dst, src, et = f[0:6], f[6:12], struct.unpack("!H", f[12:14])[0]
        if et == 0x0806 and len(f) >= 42 and f[20:22] == b"\x00\x01" and f[38:42] == GW_IP:
            self.stats["arp"] += 1
            arp = struct.pack("!HHBBH", 1, 0x0800, 6, 4, 2) + GW_MAC + GW_IP + f[22:28] + f[28:32]
            self.emit(src + GW_MAC + b"\x08\x06" + arp)
        elif et == 0x0800 and len(f) >= 34:
            ip = f[14:]; proto = ip[9]; ihl = (ip[0] & 15) * 4
            if proto == 17 and len(ip) >= ihl + 8:
                sport, dport = struct.unpack("!HH", ip[ihl:ihl + 4])
                data = ip[ihl + 8:]
                if dport == 67 and len(data) > 240:
                    opts = data[240:]; kind = 0; i = 0
                    while i < len(opts) and opts[i] != 255:
                        if opts[i] == 0: i += 1; continue
                        if opts[i] == 53: kind = opts[i + 2]
                        i += 2 + opts[i + 1]
                    name = {1: "DISCOVER", 3: "REQUEST"}.get(kind, str(kind))
                    self.stats["dhcp"].append(name)
                    log("net: DHCP", name, "from", src.hex(":"))
                    if kind in (1, 3):
                        rep = dhcp_reply(data, 2 if kind == 1 else 5)
                        self.emit(src + GW_MAC + b"\x08\x00" + udp(GW_IP, b"\xff\xff\xff\xff", 67, 68, rep))
            elif proto == 1 and ip[16:20] == GW_IP and ip[ihl] == 8:
                self.stats["ping"] += 1
                icmp = bytearray(ip[ihl:]); icmp[0] = 0; icmp[2:4] = b"\0\0"
                icmp[2:4] = struct.pack("!H", csum(bytes(icmp)))
                self.emit(src + GW_MAC + b"\x08\x00" + ip_packet(1, GW_IP, ip[12:16], bytes(icmp)))

# ── usbredir ──
T_HELLO, T_CONNECT, T_DISCONNECT, T_RESET, T_IFINFO, T_EPINFO = 0, 1, 2, 3, 4, 5
T_SETCFG, T_GETCFG, T_CFGSTATUS, T_SETALT, T_GETALT, T_ALTSTATUS = 6, 7, 8, 9, 10, 11
T_START_INT, T_STOP_INT, T_INT_STATUS = 15, 16, 17
T_CANCEL, T_FILTER_REJECT, T_FILTER_FILTER, T_DISC_ACK = 21, 22, 23, 24
T_START_BULKRX, T_STOP_BULKRX = 25, 26
T_CONTROL, T_BULK, T_ISO, T_INTERRUPT = 100, 101, 102, 103
CAPS = (1 << 1) | (1 << 4) | (1 << 5) | (1 << 6)   # + 64-bit ids, 32-bit bulk lengths (what QEMU needs for xHCI)

class Redir:
    def __init__(self, sock):
        self.s = sock; self.buf = b""
        self.chip = Chip(); self.net = Net(self.queue_rx)
        self.cfg = 0; self.rxq = []; self.pending_in = []; self.peer_caps = 0
        self.ctl_n = 0; self.bulk_out = 0; self.bulk_in = 0; self.got_hello = False
    def send(self, t, hdr=b"", data=b"", id_=0):
        # (ids are 64-bit once both hellos said so; the hellos themselves: 32-bit)
        h = struct.pack("<IIQ", t, len(hdr) + len(data), id_) if self.ids64() else struct.pack("<III", t, len(hdr) + len(data), id_)
        self.s.sendall(h + hdr + data)
    def ids64(self):
        return self.got_hello and (self.peer_caps & CAPS & (1 << 5)) != 0
    def hello(self):
        self.send(T_HELLO, b"rtl8152emu 1.0".ljust(64, b"\0") + struct.pack("<I", CAPS))
    def announce(self):
        if self.cfg == 2:
            ifs = [(0, 2, 6, 0), (1, 0x0a, 0, 0)]
            eps = {0x83: (3, 8, 0, 16), 0x81: (2, 0, 1, 512), 0x02: (2, 0, 1, 512)}
        else:
            ifs = [(0, 0xff, 0xff, 0)]
            eps = {0x81: (2, 0, 0, 512), 0x02: (2, 0, 0, 512), 0x83: (3, 8, 0, 2)}
        a = [bytearray(32) for _ in range(4)]
        for i, (n, c, s, p) in enumerate(ifs): a[0][i], a[1][i], a[2][i], a[3][i] = n, c, s, p
        self.send(T_IFINFO, struct.pack("<I", len(ifs)) + b"".join(bytes(x) for x in a))
        typ = bytearray([255] * 32); itv = bytearray(32); itf = bytearray(32); mps = [0] * 32
        typ[0] = typ[16] = 0; mps[0] = mps[16] = 64
        for ep, (t, iv, i, m) in eps.items():
            k = ((ep & 0x80) >> 3) | (ep & 0x0f)
            typ[k], itv[k], itf[k], mps[k] = t, iv, i, m
        self.send(T_EPINFO, bytes(typ) + bytes(itv) + bytes(itf) + struct.pack("<32H", *mps))
    def connect(self):
        self.announce()
        self.send(T_CONNECT, struct.pack("<BBBBHHH", 2, 0, 0, 0, 0x0bda, 0x8152, 0x2000))
        log("usb: connected as 0bda:8152 (high speed)")
    def queue_rx(self, frame):
        self.rxq.append(frame)
        self.pump()
    def pump(self):
        while self.pending_in and self.rxq and self.chip.rx_on:
            id_, ln = self.pending_in.pop(0)
            out = b""
            while self.rxq:
                f = self.rxq[0] + b"\xde\xad\xbe\xef"                # (+ the CRC the chip leaves on)
                rec = struct.pack("<I", len(f)) + b"\0" * 20 + f
                rec += b"\0" * ((8 - len(rec) % 8) % 8)
                if len(out) + len(rec) > ln: break
                out += rec; self.rxq.pop(0)
            self.bulk_in += 1
            self.send(T_BULK, struct.pack("<BBHIH", 0x81, 0, len(out) & 0xffff, 0, len(out) >> 16), out, id_)
    def control(self, id_, h, data):
        ep, req, rtype, _st, val, idx, ln = struct.unpack("<BBBBHHH", h)
        self.ctl_n += 1
        reply, status = b"", 0
        if rtype & 0x60 == 0x40:                                      # vendor: the registers
            space = idx & 0xff00
            if req != 5 or space not in (0, 0x100): status = 4
            elif rtype & 0x80: reply = self.chip.read(space, val, ln)
            else:
                en = (idx & 0xff) if ln == 8 else (idx & 0x0f)
                self.chip.write(space, val, en, data)
        elif rtype & 0x60 == 0:
            if req == 6:                                              # GET_DESCRIPTOR
                kind, i = val >> 8, val & 0xff
                if kind == 1: reply = dev_desc()
                elif kind == 2: reply = cfg1() if i == 0 else cfg2() if i == 1 else b""
                elif kind == 3: reply = string_desc(i)
                elif kind == 6: status = 4                            # (no qualifier: not dual speed here)
                else: status = 4
                if not reply and status == 0: status = 4
                reply = reply[:ln]
            elif req == 0: reply = b"\0\0"[:ln]                       # GET_STATUS
            elif req in (1, 3): pass                                  # CLEAR / SET_FEATURE
            else: log("usb: standard request %02x %02x unhandled" % (rtype, req)); status = 4
        else: status = 4
        if status: log("usb: control %02x %02x %04x %04x -> stall" % (rtype, req, val, idx))
        self.send(T_CONTROL, struct.pack("<BBBBHHH", ep, req, rtype, status, val, idx, len(reply) if rtype & 0x80 else ln), reply if rtype & 0x80 else b"", id_)
    def packet(self, t, id_, body):
        if t == T_HELLO:
            self.peer_caps = struct.unpack_from("<I", body, 64)[0] if len(body) >= 68 else 0
            self.got_hello = True
            log("usb: QEMU says hello (%s)" % body[:64].rstrip(b"\0").decode(errors="replace"))
            self.connect()
        elif t == T_RESET:
            log("usb: bus reset"); self.chip = Chip(); self.net.emit = self.queue_rx; self.rxq.clear()
            for id2, _ in self.pending_in: self.send(T_BULK, struct.pack("<BBHIH", 0x81, 1, 0, 0, 0), b"", id2)
            self.pending_in.clear()
        elif t == T_SETCFG:
            self.cfg = body[0]
            log("usb: SET_CONFIGURATION", self.cfg)
            self.announce()
            self.send(T_CFGSTATUS, struct.pack("<BB", 0, self.cfg), b"", id_)
        elif t == T_GETCFG: self.send(T_CFGSTATUS, struct.pack("<BB", 0, self.cfg), b"", id_)
        elif t == T_SETALT:
            log("usb: SET_INTERFACE", body[0], body[1])
            self.send(T_ALTSTATUS, struct.pack("<BBB", 0, body[0], body[1]), b"", id_)
        elif t == T_GETALT: self.send(T_ALTSTATUS, struct.pack("<BBB", 0, body[0], 0), b"", id_)
        elif t == T_START_INT: self.send(T_INT_STATUS, struct.pack("<BB", 0, body[0]), b"", id_)
        elif t == T_STOP_INT: self.send(T_INT_STATUS, struct.pack("<BB", 0, body[0]), b"", id_)
        elif t == T_CANCEL:
            for k, (id2, _) in enumerate(self.pending_in):
                if id2 == id_:
                    self.pending_in.pop(k)
                    self.send(T_BULK, struct.pack("<BBHIH", 0x81, 1, 0, 0, 0), b"", id_)
                    break
        elif t == T_CONTROL: self.control(id_, body[:10], body[10:])
        elif t == T_BULK:
            ep, st, ln, sid, lh = struct.unpack_from("<BBHIH", body, 0); ln |= lh << 16
            data = body[10:]
            if ep & 0x80:
                self.pending_in.append((id_, ln)); self.pump()
            else:
                self.bulk_out += 1
                if len(data) >= 8:
                    w0 = struct.unpack_from("<I", data, 0)[0]
                    flen = w0 & 0x3ffff
                    if not (w0 >> 31 & 1 and w0 >> 30 & 1): log("tx: header %08x without FS/LS" % w0)
                    self.net.frame_from_guest(data[8:8 + flen])
                self.send(T_BULK, struct.pack("<BBHIH", ep, 0, ln & 0xffff, 0, ln >> 16), b"", id_)
        elif t in (T_FILTER_FILTER, T_FILTER_REJECT, T_DISC_ACK): pass
        else: log("usb: packet type", t, "ignored")
    def feed(self, data):
        self.buf += data
        while True:
            hs = 16 if self.ids64() else 12
            if len(self.buf) < hs: break
            t, ln, id_ = struct.unpack_from("<IIQ" if hs == 16 else "<III", self.buf, 0)
            if len(self.buf) < hs + ln: break
            body = self.buf[hs:hs + ln]; self.buf = self.buf[hs + ln:]
            self.packet(t, id_, body)

def main():
    srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", PORT)); srv.listen(1)
    log("waiting for QEMU on port", PORT)
    c, _ = srv.accept()
    r = Redir(c); r.hello()
    last = time.time()
    while True:
        rd, _, _ = select.select([c], [], [], 0.05)
        if rd:
            d = c.recv(65536)
            if not d: break
            r.feed(d)
        r.pump()
        if time.time() - last > 5:
            last = time.time()
            log("stats: control %d, bulk out %d, bulk in %d, register writes %d, net %s" %
                (r.ctl_n, r.bulk_out, r.bulk_in, r.chip.writes, r.net.stats))
    log("QEMU went away; net", r.net.stats)

main()
