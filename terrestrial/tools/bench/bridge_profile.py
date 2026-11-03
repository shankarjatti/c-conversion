#!/usr/bin/env python3
"""Profile a running terrestrial bridge's ZeroMQ output so the Python and the C++ bridge can be compared on the same replay.

  python3 tools/bench/bridge_profile.py [seconds=60]

Per band: audio messages/s, seconds of decoded voice (audio messages flagged active, n/rate each), spectrum frames/s,
share of spectrum frames flagged active, mean rms_db, mean floor_db, most frequent best channel; plus the
`terr/recording` events (count, summed duration) and `terr/tetra` events seen.
"""
import collections
import json
import os
import sys
import time

import zmq

SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 60
ctx = zmq.Context()
sub = ctx.socket(zmq.SUB)
sub.setsockopt(zmq.RCVHWM, 200000)
sub.connect(os.environ.get('SECURECOM_PUB_ADDR', 'tcp://127.0.0.1:5560'))
sub.setsockopt(zmq.SUBSCRIBE, b'')
aud = collections.defaultdict(lambda: {'n': 0, 'voice_s': 0.0})
iq = collections.defaultdict(lambda: {'n': 0, 'act': 0, 'rms': 0.0, 'floor': [], 'best': collections.Counter(), 'occ': 0})
rec, tetra = [], 0
t0 = time.time()
while time.time() - t0 < SECS:
    if not sub.poll(500):
        continue
    t, h, p = sub.recv_multipart()
    t, h = t.decode(), json.loads(h)
    if t.startswith('terr/audio/'):
        a = aud[h['band']]
        a['n'] += 1
        if h['active']:
            a['voice_s'] += h['n'] / h['rate']
    elif t.startswith('terr/iq/'):
        q = iq[h['band']]
        q['n'] += 1
        q['act'] += bool(h['active'])
        q['rms'] += h['rms_db']
        if h['floor_db'] is not None:
            q['floor'].append(h['floor_db'])
        if h['active']:
            q['best'][h['chan_best']] += 1
            q['occ'] += len(h['chan_occupied'])
    elif t == 'terr/recording':
        rec.append(h['duration'])
    elif t == 'terr/tetra':
        tetra += 1
el = time.time() - t0
print(f"{'band':9s} {'aud/s':>6s} {'voice s/20 s':>12s} {'iq/s':>5s} {'active%':>8s} {'rms dB':>7s} {'floor dB':>8s} {'best ch':>8s} {'occ/frame':>9s}")
for b in ('dmr', 'nxdn', 'p25', 'tetra', 'nxdn48', 'dstar', 'x2tdma', 'provoice'):
    a, q = aud[b], iq[b]
    nq = max(q['n'], 1)
    best = q['best'].most_common(1)[0][0] if q['best'] else '-'
    fl = f"{sum(q['floor']) / len(q['floor']):.1f}" if q['floor'] else '-'
    print(f"{b:9s} {a['n'] / el:6.1f} {a['voice_s'] / el * 20:12.2f} {q['n'] / el:5.1f} {100 * q['act'] / nq:8.1f} "
          f"{q['rms'] / nq:7.1f} {fl:>8s} {str(best):>8s} {q['occ'] / max(q['act'], 1):9.2f}")
print(f'recordings saved: {len(rec)} ({sum(rec):.1f} s)   tetra events: {tetra}   window {el:.0f} s')
