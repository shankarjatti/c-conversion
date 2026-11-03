#!/usr/bin/env python3
"""Decoded voice per protocol, measured on the decoder's raw audio taps (ZeroMQ 9101-9109, float32).
DSD emits exact silence unless it decoded voice, so non-zero samples = decoded voice.

  python3 tools/bench/voice_rates.py [seconds=60]   -> seconds of voice per 20 s window, per protocol
"""
import os
import sys
import threading
import time

import numpy as np
import zmq

T = float(sys.argv[1]) if len(sys.argv) > 1 else 60
TAPS = {9101: ('dmr', 48000), 9102: ('nxdn', 48000), 9103: ('p25', 48000), 9104: ('tetra', 8000),
        9106: ('nxdn48', 48000), 9107: ('dstar', 48000), 9108: ('x2tdma', 48000), 9109: ('provoice', 48000)}
SHIFT = int(os.environ.get('SECURECOM_TERR_TAP_PORT_SHIFT', '0'))      # measure a second decoder whose taps are shifted
TAPS = {p + SHIFT: v for p, v in TAPS.items()}
res = {n: 0 for n, _ in TAPS.values()}
lock = threading.Lock()


def run(port, name, rate):
    s = zmq.Context().socket(zmq.SUB)
    s.connect(f'tcp://127.0.0.1:{port}')
    s.setsockopt(zmq.SUBSCRIBE, b'')
    s.RCVTIMEO = 300
    t0 = time.time()
    while time.time() - t0 < T:
        try:
            x = np.frombuffer(s.recv(), dtype=np.float32)
        except zmq.Again:
            continue
        with lock:
            res[name] += int(np.count_nonzero(np.abs(x) > 1e-4))


ts = [threading.Thread(target=run, args=(p, n, r)) for p, (n, r) in TAPS.items()]
[t.start() for t in ts]
[t.join() for t in ts]
print(f'voice seconds per 20 s window, mean over {T:.0f} s:')
print('  ' + '  '.join(f'{n}={res[n] / r / T * 20:5.2f}' for _, (n, r) in TAPS.items()))
