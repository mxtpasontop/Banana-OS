#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A lossy Ethernet cable between a QEMU guest's NIC and QEMU's slirp network.

    lossywire.py LOSS_TO_GUEST LOSS_FROM_GUEST [SEED]
    qemu ... -netdev dgram,id=g,local.type=inet,local.host=127.0.0.1,local.port=9001,remote.type=inet,remote.host=127.0.0.1,remote.port=9002
             -device e1000,netdev=g
             -netdev user,id=u -netdev hubport,id=h1,hubid=0,netdev=u
             -netdev dgram,id=s,local.type=inet,local.host=127.0.0.1,local.port=9003,remote.type=inet,remote.host=127.0.0.1,remote.port=9004
             -netdev hubport,id=h2,hubid=0,netdev=s

Frames from the guest arrive on 9002 and go on to slirp (9003); slirp's
answers arrive on 9004 and go to the guest (9001). A share of each way is
dropped (and now and then two frames swapped), as a real network does.
"""
import random, select, socket, sys, time

loss_in = float(sys.argv[1]) if len(sys.argv) > 1 else 0.03
loss_out = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
random.seed(int(sys.argv[3]) if len(sys.argv) > 3 else 1)
g = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); g.bind(("127.0.0.1", 9002))
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("127.0.0.1", 9004))
st = {"to_guest": 0, "dropped_to_guest": 0, "from_guest": 0, "dropped_from_guest": 0, "swapped": 0}
held = None
last = time.time()
while True:
    r, _, _ = select.select([g, s], [], [], 0.2)
    for sock in r:
        d, _ = sock.recvfrom(65536)
        if sock is g:
            st["from_guest"] += 1
            if random.random() < loss_out: st["dropped_from_guest"] += 1; continue
            s.sendto(d, ("127.0.0.1", 9003))
        else:
            st["to_guest"] += 1
            if len(d) > 200 and random.random() < loss_in: st["dropped_to_guest"] += 1; continue
            if len(d) > 200 and held is None and random.random() < 0.01:   # reordered: this one after the next
                held = d; st["swapped"] += 1; continue
            g.sendto(d, ("127.0.0.1", 9001))
            if held is not None: g.sendto(held, ("127.0.0.1", 9001)); held = None
    if time.time() - last > 5:
        last = time.time(); print(st, flush=True)
