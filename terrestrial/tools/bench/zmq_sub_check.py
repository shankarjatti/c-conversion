#!/usr/bin/env python3
"""Check the backend's ZeroMQ interface (gui/zmq_iface.py) against a RUNNING backend. Changes nothing except
one throw-away test file it creates (and deletes through the delete command).

  python3 tools/bench/zmq_sub_check.py [seconds=25] [--decoder-cycle]

Phase A  subscribe to everything for N s: topics seen, message rates, sequence gaps per topic, header and payload
         sizes (audio: 2*n bytes; iq: 4*(n_spec+n_time+n_chan) bytes), timestamps.
Phase B  commands: get_state, list_recordings, get_recording (bytes == the file on disk), delete_recording,
         unknown command, and folder-escape attempts that must be refused.
Phase C  (--decoder-cycle) stop_decoder -> `terr/decoder` event -> start_decoder -> audio flows again.
"""
import collections
import json
import os
import struct
import sys
import time
import wave

import zmq

PUB = os.environ.get('SECURECOM_PUB_ADDR', 'tcp://127.0.0.1:5560')
CMD = os.environ.get('SECURECOM_CMD_ADDR', 'tcp://127.0.0.1:5561')
SECS = float(next((a for a in sys.argv[1:] if not a.startswith('-')), 25))
REC = os.environ.get('SECURECOM_RECORDINGS_DIR') or os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'gui', 'recordings'))
results = []


def check(name, ok, detail=''):
    results.append((name, bool(ok), detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ''), flush=True)


def main():
    ctx = zmq.Context()
    sub = ctx.socket(zmq.SUB)
    sub.setsockopt(zmq.RCVHWM, 100000)
    sub.connect(PUB)
    sub.setsockopt(zmq.SUBSCRIBE, b'')
    req = ctx.socket(zmq.REQ)
    req.setsockopt(zmq.RCVTIMEO, 8000)
    req.setsockopt(zmq.LINGER, 0)
    req.connect(CMD)

    def call(**kw):
        req.send(json.dumps(kw).encode())
        frames = req.recv_multipart()
        return json.loads(frames[0]), (frames[1] if len(frames) > 1 else b'')

    print(f'=== Phase A: subscribe to {PUB} for {SECS:.0f} s')
    stats = collections.defaultdict(lambda: {'n': 0, 'last': None, 'gaps': 0, 'lost': 0, 'bytes': 0, 'bad': 0, 'ex': None})
    t0 = time.time()
    skew = []
    while time.time() - t0 < SECS:
        if not sub.poll(500):
            continue
        frames = sub.recv_multipart()
        if len(frames) != 3:
            stats['<bad framing>']['bad'] += 1
            continue
        topic, hdr_raw, payload = frames
        topic = topic.decode()
        s = stats[topic]
        try:
            h = json.loads(hdr_raw)
        except ValueError:
            s['bad'] += 1
            continue
        s['n'] += 1
        s['bytes'] += len(payload)
        if s['ex'] is None:
            s['ex'] = {k: v for k, v in h.items() if k not in ('items', 'bands')}
        seq = h.get('seq')
        if s['last'] is not None and seq != s['last'] + 1:
            s['gaps'] += 1
            s['lost'] += max(0, seq - s['last'] - 1)
        s['last'] = seq
        skew.append(abs(time.time() - h.get('t', 0)))
        if topic.startswith('terr/audio/'):
            if len(payload) != 2 * h.get('n', -1):
                s['bad'] += 1
        elif topic.startswith('terr/iq/'):
            if len(payload) != 4 * (h['n_spec'] + h['n_time'] + h['n_chan']):
                s['bad'] += 1
    el = time.time() - t0
    print(f"{'topic':22s} {'msgs':>7s} {'msg/s':>7s} {'KB/s':>8s} {'seq gaps':>9s} {'lost':>6s} {'bad':>4s}")
    for tpc in sorted(stats):
        s = stats[tpc]
        print(f"{tpc:22s} {s['n']:7d} {s['n'] / el:7.1f} {s['bytes'] / 1024 / el:8.1f} {s['gaps']:9d} {s['lost']:6d} {s['bad']:4d}")
    bands = ['dmr', 'nxdn', 'p25', 'tetra', 'nxdn48', 'dstar', 'x2tdma', 'provoice']
    check('A1 audio topic for all 8 bands', all(f'terr/audio/{b}' in stats for b in bands))
    check('A2 iq (spectrum) topic for all 8 bands', all(f'terr/iq/{b}' in stats for b in bands))
    check('A3 state topics present (status, bands, recordings)', all(t in stats for t in ('terr/status', 'terr/bands', 'terr/recordings')))
    check('A4 no malformed message (framing, JSON, payload size)', all(s['bad'] == 0 for s in stats.values()))
    lost_audio = sum(stats[f'terr/audio/{b}']['lost'] for b in bands if f'terr/audio/{b}' in stats)
    total_audio = sum(stats[f'terr/audio/{b}']['n'] for b in bands if f'terr/audio/{b}' in stats)
    check('A5 sequence numbers: audio loss below 1 %', total_audio > 0 and lost_audio <= 0.01 * (total_audio + lost_audio),
          f'{lost_audio} lost of {total_audio + lost_audio}')
    check('A6 timestamps within 5 s of now', skew and max(skew) < 5, f'max {max(skew):.2f} s' if skew else 'no messages')

    print('=== Phase B: commands on', CMD)
    h, _ = call(cmd='get_state')
    check('B1 get_state (status, 8 bands, recordings_total)', h.get('ok') and len(h.get('bands', [])) == 8 and 'status' in h,
          f"decoder_running={h.get('status', {}).get('decoder_running')} iq_connected={sum(h.get('status', {}).get('iq_connected', {}).values())}/8")
    h, _ = call(cmd='list_recordings', limit=3)
    check('B2 list_recordings (paged)', h.get('ok') and 'total' in h and len(h.get('items', [])) <= 3, f"total={h.get('total')}")
    # a throw-away recording we create ourselves
    os.makedirs(REC, exist_ok=True)
    name = f'zmqtest_{int(time.time())}.wav'
    path = os.path.join(REC, name)
    with wave.open(path, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(8000)
        w.writeframes(struct.pack('<2000h', *range(-1000, 1000)))
    disk = open(path, 'rb').read()
    h, data = call(cmd='list_recordings', limit=500)
    check('B3 a new file shows up in list_recordings', any(i['file'] == name for i in h.get('items', [])))
    got, off = b'', 0
    while True:
        h, data = call(cmd='get_recording', file=name, offset=off, length=1000)
        if not h.get('ok'):
            break
        got += data
        off += len(data)
        if h.get('eof'):
            break
    check('B4 get_recording in 1000-byte chunks == the file on disk', got == disk, f'{len(got)} of {len(disk)} bytes')
    h, _ = call(cmd='get_recording', file='../../../../etc/passwd')
    check('B5 folder-escape download refused', not h.get('ok'))
    outside = os.path.join(REC, '..', 'zmq_escape_probe.wav.txt')      # a file OUTSIDE the recordings folder that must survive
    open(outside, 'w').close()
    h, _ = call(cmd='delete_recording', file='../zmq_escape_probe.wav.txt')
    survived = os.path.exists(outside)
    os.remove(outside)
    check('B6 folder-escape delete refused', not h.get('ok') and survived)
    h, _ = call(cmd='delete_recording', file=name)
    check('B7 delete_recording removes the file', h.get('ok') and h.get('deleted') and not os.path.exists(path))
    h, _ = call(cmd='no_such_command')
    check('B8 unknown command answered with an error (backend stays up)', not h.get('ok') and 'known' in h)
    h, _ = call(cmd='get_state')
    check('B9 backend still answers afterwards', h.get('ok'))

    if '--decoder-cycle' in sys.argv:
        print('=== Phase C: stop / start the decoder over ZeroMQ')
        h, _ = call(cmd='stop_decoder')
        ev = None
        t1 = time.time()
        while time.time() - t1 < 8 and ev is None:
            if sub.poll(500):
                f = sub.recv_multipart()
                if f[0] == b'terr/decoder':
                    ev = json.loads(f[1])
        check('C1 stop_decoder -> terr/decoder running=false event', h.get('decoder_running') is False and ev and ev.get('running') is False)
        h, _ = call(cmd='start_decoder')
        t_restart = time.time()
        check('C2 start_decoder', h.get('decoder_running') is True)
        # only audio PRODUCED after the restart counts (older messages may still be queued in the socket)
        back = None
        while time.time() - t_restart < 90 and back is None:
            if sub.poll(500):
                f = sub.recv_multipart()
                if f[0].startswith(b'terr/audio/'):
                    hh = json.loads(f[1])
                    if hh.get('active') and hh['t'] > t_restart:
                        back = time.time() - t_restart
        check('C3 decoded audio (made after the restart) flows again', back is not None,
              f'{back:.0f} s after start_decoder' if back is not None else 'no new decoded audio in 90 s')

    bad = [r for r in results if not r[1]]
    print(f'\n{len(results) - len(bad)}/{len(results)} checks passed')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
