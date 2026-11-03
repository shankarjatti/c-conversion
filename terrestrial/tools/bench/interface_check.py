#!/usr/bin/env python3
"""Check a RUNNING TERRESTRIAL module against docs/INTERFACE.md (and the shared contract ../docs/INTERFACES_V1.md).

  python3 tools/bench/interface_check.py [seconds=20]        (start it with: tools/run_terrestrial.sh start --replay --test)

Listens to every result stream for N seconds and validates each message: framing, header fields and types, sequence numbers,
payload sizes, known topics; then exercises the commands of the bridge (including the refusals). Exit 0 = all rules hold.
"""
import collections
import json
import os
import sys
import time

import zmq

SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 20
TERR_BANDS = {'dmr', 'nxdn', 'p25', 'tetra', 'nxdn48', 'dstar', 'x2tdma', 'provoice'}
ALL_BANDS = TERR_BANDS
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
    if parts[0] == 'terr':
        if len(parts) == 3 and parts[1] == 'audio' and parts[2] in TERR_BANDS:
            need(h, topic, band=str, rate=int, active=bool, rms_db=num, n=int)
            if isinstance(h.get('n'), int) and len(p) != 2 * h['n']:
                bad('terr/audio payload = 2 x n bytes', f'{len(p)} vs n={h["n"]}')
        elif len(parts) == 3 and parts[1] == 'iq' and parts[2] in TERR_BANDS:
            need(h, topic, band=str, active=bool, rms_db=num, n_spec=int, n_time=int, n_chan=int, chan_best=int, chan_occupied=list, chan_bw_hz=int)
            if all(isinstance(h.get(k), int) for k in ('n_spec', 'n_time', 'n_chan')) and len(p) != 4 * (h['n_spec'] + h['n_time'] + h['n_chan']):
                bad('terr/iq payload = 4 x (n_spec+n_time+n_chan) bytes', f'{len(p)}')
        elif topic == 'terr/status':
            need(h, topic, source=str, decoder_running=bool, iq_connected=dict, iq_transport=str)
        elif topic == 'terr/bands':
            need(h, topic, bands=list)
            for b in h.get('bands', []):
                for k in ('key', 'name', 'freq_mhz', 'iq_rate_sps', 'channels', 'chan_bw_hz', 'audio_topic', 'audio_rate_hz', 'iq_topic'):
                    if k not in b:
                        bad('terr/bands entry has all fields', f'missing {k}')
        elif topic == 'terr/decoder':
            need(h, topic, running=bool)
        elif topic == 'terr/log':
            need(h, topic, text=str)
        elif topic == 'terr/recording':
            need(h, topic, band=str, file=str, duration=num, had_audio=bool, peak_db=num)
        elif topic == 'terr/recordings':
            need(h, topic, total=int, items=list)
        elif topic == 'terr/tetra':
            need(h, topic, data=dict)
        else:
            bad('only documented terr/ topics', topic)
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
    for name, addr in (('terr', 'tcp://127.0.0.1:5560'), ('ingest', 'tcp://127.0.0.1:' + os.environ.get('SECURECOM_IQ_PORT', '5570'))):
        s = ctx.socket(zmq.SUB)
        s.setsockopt(zmq.RCVHWM, 200000)
        s.connect(addr)
        s.setsockopt(zmq.SUBSCRIBE, b'')
        poll.register(s, zmq.POLLIN)
        socks[s] = name
    print(f'listening to the terrestrial results (5560) and the Ingest output for {SECS:.0f} s ...', flush=True)
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

    want = ['terr/audio', 'terr/iq', 'terr/status', 'terr/bands', 'terr/log', 'terr/recordings', 'iq', 'ingest/status']
    got = {t.split('/')[0] + '/' + t.split('/')[1] if '/' in t else t for t in seen}
    for w in want:
        check(f'topic family present: {w}', any(g == w or g.startswith(w) for g in got) or any(t.startswith(w) for t in seen))
    check('no malformed message and every header/payload rule holds', not violations, '; '.join(f'{k}: {v}x e.g. {examples[k]}' for k, v in list(violations.items())[:5]))
    check('no sequence gap on the result streams', not gaps.get('terr'), str(dict(gaps)))

    def call(addr, **kw):
        r = ctx.socket(zmq.REQ)
        r.setsockopt(zmq.RCVTIMEO, 8000)
        r.setsockopt(zmq.LINGER, 0)
        r.connect(addr)
        r.send(json.dumps(kw).encode())
        fr = r.recv_multipart()
        r.close(0)
        return json.loads(fr[0]), (fr[1] if len(fr) > 1 else b'')

    T = 'tcp://127.0.0.1:5561'
    h, _ = call(T, cmd='get_state')
    check('terr get_state: ok, interface_version 1.0, status, 8 bands, recordings_total', h.get('ok') and h.get('interface_version') == '1.0' and 'status' in h and len(h.get('bands', [])) == 8 and 'recordings_total' in h)
    h, _ = call(T, cmd='list_recordings', limit=3)
    check('terr list_recordings is paged', h.get('ok') and 'total' in h and len(h.get('items', [])) <= 3)
    h, _ = call(T, cmd='get_recording', file='../../../../etc/passwd')
    check('terr get_recording refuses a folder escape', not h.get('ok'))
    h, _ = call(T, cmd='delete_recording', file='../bridge_server.py')
    check('terr delete_recording refuses a non-wav / escape', not h.get('ok'))
    h, _ = call(T, cmd='no_such_command')
    check('terr unknown command -> ok:false + known', not h.get('ok') and 'known' in h)
    print(f'\n{sum(results)}/{len(results)} checks passed')
    sys.exit(0 if all(results) else 1)


if __name__ == '__main__':
    main()
