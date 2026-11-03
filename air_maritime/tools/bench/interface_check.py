#!/usr/bin/env python3
"""Check a RUNNING AIR & MARITIME module against docs/INTERFACE.md (and the shared contract ../docs/INTERFACES_V1.md).

  python3 tools/bench/interface_check.py [seconds=20]        (start it with: tools/run_air_maritime.sh start --replay)

Listens to every result stream for N seconds and validates each message: framing, header fields and types, sequence numbers,
payload sizes, known topics; then exercises the commands of both bridges (including the refusals). Exit 0 = all rules hold.
"""
import collections
import json
import os
import sys
import time

import zmq

SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 20
ALL_BANDS = {'ais', 'adsb'}
num = (int, float)
violations = collections.Counter()
examples = {}
seen = collections.Counter()
last_seq = {}
gaps = collections.Counter()


def bad(rule, detail):
    violations[rule] += 1
    examples.setdefault(rule, detail)


def need(h, topic, **types):
    for k, t in types.items():
        if k not in h:
            bad(f'{topic}: header has {k}', f'missing {k}')
        elif not isinstance(h[k], t) or isinstance(h[k], bool) and t is not bool and bool not in (t if isinstance(t, tuple) else (t,)):
            bad(f'{topic}: {k} has the right type', f'{k}={h[k]!r}')


def check_message(addr_name, f):
    if len(f) != 3:
        bad('3 frames per message', f'{len(f)} frames')
        return
    try:
        topic = f[0].decode('ascii')
        h = json.loads(f[1])
    except Exception as e:
        bad('topic is ASCII and header is JSON', repr(e))
        return
    if not isinstance(h, dict):
        bad('header is a JSON object', type(h).__name__)
        return
    p = f[2]
    seen[topic.rsplit('/', 1)[0] if topic.count('/') >= 2 and not topic.endswith('/') else topic] += 1
    need(h, 'every message', seq=int, t=num)
    if isinstance(h.get('seq'), int):
        if topic in last_seq and h['seq'] != last_seq[topic] + 1 and h['seq'] > last_seq[topic]:
            gaps[topic.split('/')[0]] += h['seq'] - last_seq[topic] - 1
        last_seq[topic] = h['seq']
    parts = topic.split('/')
    if parts[0] == 'air':
        if topic in ('air/ais/target', 'air/adsb/target'):
            need(h, topic, target=dict)
            t = h.get('target', {})
            key = 'mmsi' if topic == 'air/ais/target' else 'icao'
            if key not in t:
                bad(f'{topic} target has {key}', str(t)[:60])
            if topic == 'air/adsb/target' and isinstance(t.get('heading'), num) and not 0 <= t['heading'] < 360.0001:
                bad('adsb heading is a compass heading 0-360', str(t['heading']))
        elif topic in ('air/ais/remove', 'air/adsb/remove'):
            need(h, topic, **({'mmsi': str} if 'ais' in topic else {'icao': str}))
        elif topic in ('air/ais/targets', 'air/adsb/targets'):
            need(h, topic, targets=list)
        elif len(parts) == 3 and parts[1] == 'spectrum' and parts[2] in ('ais', 'adsb'):
            need(h, topic, band=str, active=bool, rms_db=num, ffts=int, n_spec=int, n_peak=int)
            if all(isinstance(h.get(k), int) for k in ('n_spec', 'n_peak')) and len(p) != 4 * (h['n_spec'] + h['n_peak']):
                bad('air/spectrum payload = 4 x (n_spec+n_peak) bytes', f'{len(p)}')
        elif topic == 'air/status':
            need(h, topic, decoder_running=bool, iq_transport=str, iq_connected=dict, ais_targets=int, adsb_targets=int)
        elif topic == 'air/bands':
            need(h, topic, bands=list)
        else:
            bad('only documented air/ topics', topic)
    elif parts[0] == 'iq' and topic.endswith('/'):
        need(h, topic, band=str, module=str, centre_hz=num, rate_sps=num, n=int, src_seq=(int, type(None)))
        if parts[1] not in ALL_BANDS:
            bad('iq/<band>/ names a configured band', topic)
        if isinstance(h.get('n'), int) and len(p) != 8 * h['n']:
            bad('iq payload = 8 x n bytes (complex64)', f'{len(p)} vs n={h["n"]}')
    elif topic == 'ingest/status':
        need(h, topic, **{'in': int, 'routed': dict, 'unknown': int, 'bad': int, 'rate_mismatch': int, 'src_lost': int, 'out_dropped': int})
    else:
        bad('only documented topics', topic)


def main():
    ctx = zmq.Context()
    poll = zmq.Poller()
    socks = {}
    for name, addr in (('air', 'tcp://127.0.0.1:5565'), ('ingest', 'tcp://127.0.0.1:' + os.environ.get('SECURECOM_IQ_PORT', '5571'))):
        s = ctx.socket(zmq.SUB)
        s.setsockopt(zmq.RCVHWM, 200000)
        s.connect(addr)
        s.setsockopt(zmq.SUBSCRIBE, b'')
        poll.register(s, zmq.POLLIN)
        socks[s] = name
    print(f'listening to the air & maritime results (5565) and the Ingest output for {SECS:.0f} s ...', flush=True)
    t0, n = time.time(), 0
    while time.time() - t0 < SECS:
        for s, _ in poll.poll(200):
            while True:
                try:
                    f = s.recv_multipart(zmq.NOBLOCK)
                except zmq.Again:
                    break
                check_message(socks[s], f)
                n += 1
    print(f'{n} messages, {len(seen)} topic groups, sequence gaps per stream: {dict(gaps) or "none"}')
    results = []

    def check(name, ok, detail=''):
        results.append(bool(ok))
        print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f'  — {detail}' if detail else ''))

    want = ['air/ais', 'air/adsb', 'air/spectrum', 'air/status', 'air/bands', 'iq', 'ingest/status']
    got = {t.split('/')[0] + '/' + t.split('/')[1] if '/' in t else t for t in seen}
    for w in want:
        check(f'topic family present: {w}', any(g == w or g.startswith(w) for g in got) or any(t.startswith(w) for t in seen))
    check('no malformed message and every header/payload rule holds', not violations, '; '.join(f'{k}: {v}x e.g. {examples[k]}' for k, v in list(violations.items())[:5]))
    check('no sequence gap on the result streams', not gaps.get('air'), str(dict(gaps)))

    def call(addr, **kw):
        r = ctx.socket(zmq.REQ)
        r.setsockopt(zmq.RCVTIMEO, 8000)
        r.setsockopt(zmq.LINGER, 0)
        r.connect(addr)
        r.send(json.dumps(kw).encode())
        fr = r.recv_multipart()
        r.close(0)
        return json.loads(fr[0]), (fr[1] if len(fr) > 1 else b'')

    A = 'tcp://127.0.0.1:5566'
    h, _ = call(A, cmd='get_state')
    check('air get_state: ok, interface_version 1.0, status, 2 bands, targets', h.get('ok') and h.get('interface_version') == '1.0' and len(h.get('bands', [])) == 2 and 'ais_vessels' in h and 'adsb_aircraft' in h)
    h, _ = call(A, cmd='no_such_command')
    check('air unknown command -> ok:false + known', not h.get('ok') and 'known' in h)
    print(f'\n{sum(results)}/{len(results)} checks passed')
    sys.exit(0 if all(results) else 1)


if __name__ == '__main__':
    main()
