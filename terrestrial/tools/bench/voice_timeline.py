#!/usr/bin/env python3
"""Decoded voice of one band as a timeline: one character per second of the tap (0 = silent ... 9 = voice the whole second).
  python3 tools/bench/voice_timeline.py nxdn [seconds=90]"""
import sys
import time

import numpy as np
import zmq

PORTS = {'dmr': 9101, 'nxdn': 9102, 'p25': 9103, 'nxdn48': 9106, 'dstar': 9107}
band = sys.argv[1]
T = float(sys.argv[2]) if len(sys.argv) > 2 else 90
s = zmq.Context().socket(zmq.SUB)
s.connect(f'tcp://127.0.0.1:{PORTS[band]}')
s.setsockopt(zmq.SUBSCRIBE, b'')
s.RCVTIMEO = 300
t0 = time.time()
bins = {}
while time.time() - t0 < T:
    try:
        x = np.frombuffer(s.recv(), dtype=np.float32)
    except zmq.Again:
        continue
    k = int(time.time() - t0)
    v = bins.setdefault(k, [0, 0])
    v[0] += int(np.count_nonzero(np.abs(x) > 1e-4))
    v[1] += len(x)
line = ''.join(str(min(9, int(10 * bins[k][0] / max(bins[k][1], 1)))) if k in bins else '.' for k in range(int(T)))
print(line)
print(f'voiced fraction {sum(v[0] for v in bins.values()) / max(sum(v[1] for v in bins.values()), 1):.3f}')
