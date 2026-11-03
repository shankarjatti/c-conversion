#!/usr/bin/env python3
"""Check the air & maritime ZeroMQ interface (docs/INTERFACE.md) against a RUNNING backend. Read-only.

  python3 tools/bench/air_zmq_check.py [seconds=30]

Subscribes to everything on PUB :5565, counts messages / sequence gaps per topic, checks spectrum payload sizes
(2 x float32 x 512), and that targets (AIS vessels, ADS-B aircraft) arrive with real fields; then the command channel.
"""
import collections
import json
import os
import sys
import time

import zmq

PUB = os.environ.get('SECURECOM_AIR_PUB_ADDR', 'tcp://127.0.0.1:5565')
CMD = os.environ.get('SECURECOM_AIR_CMD_ADDR', 'tcp://127.0.0.1:5566')
SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 30
results = []


def check(name, ok, detail=''):
    results.append(bool(ok))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''), flush=True)


ctx = zmq.Context()
sub = ctx.socket(zmq.SUB)
sub.setsockopt(zmq.RCVHWM, 100000)
sub.connect(PUB)
sub.setsockopt(zmq.SUBSCRIBE, b'')
st = collections.defaultdict(lambda: {'n': 0, 'last': None, 'lost': 0, 'bad': 0})
ais, adsb, spec_active = {}, {}, {}
bands, status = None, None
t0 = time.time()
while time.time() - t0 < SECS:
    if not sub.poll(500):
        continue
    f = sub.recv_multipart()
    if len(f) != 3:
        st['<framing>']['bad'] += 1
        continue
    topic, h, payload = f[0].decode(), json.loads(f[1]), f[2]
    s = st[topic]
    s['n'] += 1
    if s['last'] is not None and h['seq'] != s['last'] + 1:
        s['lost'] += max(0, h['seq'] - s['last'] - 1)
    s['last'] = h['seq']
    if topic.startswith('air/spectrum/'):
        if len(payload) != 4 * (h['n_spec'] + h['n_peak']):
            s['bad'] += 1
        if h['n_spec']:
            spec_active[topic] = h['active']
    elif topic == 'air/ais/target':
        ais[h['target'].get('mmsi')] = h['target']
    elif topic == 'air/adsb/target':
        adsb[h['target'].get('icao')] = h['target']
    elif topic == 'air/bands':
        bands = h['bands']
    elif topic == 'air/status':
        status = h
el = time.time() - t0
print(f"{'topic':24s} {'msgs':>6s} {'msg/s':>7s} {'lost':>5s} {'bad':>4s}")
for t in sorted(st):
    print(f"{t:24s} {st[t]['n']:6d} {st[t]['n'] / el:7.1f} {st[t]['lost']:5d} {st[t]['bad']:4d}")
print(f'AIS vessels seen: {len(ais)}   ADS-B aircraft seen: {len(adsb)}')
check('A1 spectrum topic for ais and adsb', 'air/spectrum/ais' in st and 'air/spectrum/adsb' in st)
check('A2 status + bands state topics', 'air/status' in st and 'air/bands' in st and bands and len(bands) == 2)
check('A3 no malformed message', all(s['bad'] == 0 for s in st.values()))
check('A4 spectrum sequence loss below 1 %', all(st[t]['lost'] <= 0.01 * (st[t]['n'] + st[t]['lost']) for t in st if t.startswith('air/spectrum/')))
check('A5 AIS vessels with a real MMSI and position', any(v.get('mmsi') and v.get('lat') is not None for v in ais.values()), f'{len(ais)} vessels')
check('A6 ADS-B aircraft with ICAO address', len(adsb) > 0, f'{len(adsb)} aircraft, {sum(1 for a in adsb.values() if a.get("lat") is not None)} with position')
check('A7 IQ connected on both bands', status and all(status['iq_connected'].values()), str(status and status['iq_connected']))

req = ctx.socket(zmq.REQ)
req.setsockopt(zmq.RCVTIMEO, 8000)
req.setsockopt(zmq.LINGER, 0)
req.connect(CMD)


def call(**kw):
    req.send(json.dumps(kw).encode())
    return json.loads(req.recv_multipart()[0])


h = call(cmd='get_state')
check('B1 get_state', h.get('ok') and h['status']['decoder_running'] and len(h['bands']) == 2, f"{len(h.get('ais_vessels', []))} vessels, {len(h.get('adsb_aircraft', []))} aircraft")
h = call(cmd='no_such_command')
check('B2 unknown command answered with an error', not h.get('ok') and 'known' in h)
h = call(cmd='get_state')
check('B3 backend still answers', h.get('ok'))
print(f'\n{sum(results)}/{len(results)} checks passed')
sys.exit(0 if all(results) else 1)
